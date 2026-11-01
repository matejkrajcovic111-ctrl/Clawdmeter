#pragma once
#include <stdbool.h>

// clawdmeter2: the swipe transition. Square "pixels" swirl in from the edges
// in the shown account's colour and go dark until the whole screen is
// covered; then `swap` runs (the page changes underneath), the screen is
// redrawn in full, and the pixels open again from the centre in the new
// page's colour. One full-screen object on the top layer that draws its own
// cells and redraws only the cells that change (like field.h), so it needs
// almost no LVGL memory.

void portal_init(void);                         // after ui_init()
// dir: +1 / -1 = which way the finger went (the swirl turns that way).
// swap runs once, while the screen is fully covered. False (and swap runs
// right away) if a portal is already running.
bool portal_start(int dir, void (*swap)(void));
bool portal_active(void);
