#pragma once
#include <stdint.h>
#include <stdbool.h>

// clawdmeter2: OLED-friendly screensavers. One hidden full-screen custom-draw
// object (black background, not clickable), mostly black, peak ~70% opacity,
// everything drifts. No per-particle LVGL objects, fixed arrays, ~10-30 fps.

#define SAVER_COUNT 3

void saver_init(void* parent);     // lv_obj_t* parent (NULL = lv_layer_top())
void saver_start(int kind);        // 0 starfield, 1 code rain,
                                   // 2 constellation; fresh state
void saver_stop(void);             // hide now
bool saver_active(void);
int  saver_kind(void);             // -1 when inactive
const char* saver_name(int kind);
void saver_tick(void);             // call every loop; no-op when inactive
