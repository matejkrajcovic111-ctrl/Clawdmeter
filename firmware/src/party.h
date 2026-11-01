#pragma once
#include <stdbool.h>
#include <lvgl.h>

// clawdmeter2: party effects. One full-screen custom-draw object on the top
// layer (like portal.h): a confetti burst and an edge glow, drawn directly
// with no per-particle LVGL objects. Redraws only while something is active.

void party_init(lv_obj_t* parent);   // once; hidden, not clickable
void party_confetti(void);           // ~2.5 s burst from the bottom, fades out
void party_glow(lv_color_t color, int pulses);  // screen edge pulses, ~1.2 s each
bool party_active(void);
void party_tick(void);               // call each loop; ~30 fps cap
