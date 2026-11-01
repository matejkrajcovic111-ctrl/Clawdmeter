#pragma once
#include <stdbool.h>
#include "data.h"

// Full-screen overlay on lv_layer_top(), covering the meters: the card's
// usage drawn like Claude Code's /usage, then its history.
// Everything is allocated once in detail_init(); show/refresh only restyle.
// Dismissal is the caller's job: call detail_hide() on any tap.
enum DetailMode {
    DETAIL_CHART,    // last 30 days as bars, busiest day and 30-day total
    DETAIL_TOKENS,   // this week's tokens plus the joke line
};

void detail_init(void);                                         // creates hidden objects
void detail_show(DetailMode mode, int account, bool neon);      // account 1..9; neon = account-2 palette
void detail_hide(void);
bool detail_visible(void);
void detail_refresh(void);   // redraw if visible (e.g. new history landed)
// The shown account's numbers (the same view the meters draw); redraws if visible.
void detail_set_usage(const UsageData* view);
