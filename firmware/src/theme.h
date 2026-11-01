#pragma once
#include <lvgl.h>

// Design tokens — single source of truth for UI colors. Anthropic-inspired
// dark palette, AMOLED-friendly (true black bg).
#define THEME_BG       lv_color_hex(0x000000)   // screen background
#define THEME_PANEL    lv_color_hex(0x1f1f1e)   // card/zone fill
#define THEME_TEXT     lv_color_hex(0xfaf9f5)   // primary text
#define THEME_DIM      lv_color_hex(0xb0aea5)   // secondary text
#define THEME_ACCENT   lv_color_hex(0xd97757)   // brand terra-cotta
#define THEME_GREEN    lv_color_hex(0x788c5d)
#define THEME_AMBER    lv_color_hex(0xd97757)
#define THEME_RED      lv_color_hex(0xc0392b)
#define THEME_BAR_BG   lv_color_hex(0x2a2a28)   // unfilled bar track

// Account 2 (clawdmeter2): the same roles in blue (the NEON_ names stayed from
// the first green version). High usage stays a warning colour, a hot red, so a
// nearly spent account still reads as one.
#define NEON_PANEL     lv_color_hex(0x0a1626)
#define NEON_TEXT      lv_color_hex(0xdcebff)
#define NEON_DIM       lv_color_hex(0x7fa8d8)
#define NEON_ACCENT    lv_color_hex(0x4aa3ff)
#define NEON_GREEN     lv_color_hex(0x4aa3ff)
#define NEON_AMBER     lv_color_hex(0xffb347)
#define NEON_RED       lv_color_hex(0xff3355)
#define NEON_BAR_BG    lv_color_hex(0x12263d)
