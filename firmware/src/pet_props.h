#pragma once
#include <stdint.h>

// Little props that decorate the full-screen face in pet mode: sweat drop,
// zzz, confetti, a thinking dot and a speech pill. All objects live on
// lv_layer_top(), are created once by props_init() and hidden until asked for.
// Coordinates are screen pixels; sizes are authored for 480 px and scaled by
// min(width, height) / 480. Call props_to_front() after the face rises so
// they draw above it.

void props_init(void);                                  // once, after face_init()
void props_to_front(void);                              // raise every prop above the face
void props_set_neon(bool neon);                         // accent palette for account 2

void props_sweat(bool on, int x, int y);                // teardrop slides down from (x,y), repeating
void props_zzz(bool on, int x, int y);                  // z z Z rise up-right from (x,y), looping
void props_confetti(void);                              // one burst, falls top to bottom
void props_thinking(bool on, int cx, int cy, int r);    // dot orbits (cx,cy) at radius r
bool props_thinking_pos(int* x, int* y);                // dot centre; false when off
void props_bubble(const char* text, int x, int y, uint32_t ms);  // pill centred at (x,y)

void props_hide_all(void);                              // stop everything, hide at once
