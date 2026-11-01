#pragma once
#include <stdint.h>

// clawdmeter2: while account 2 is on screen Clawd turns blue. Red and blue
// swap places in his RGB565 palette (terra-cotta becomes sky blue; the eyes,
// white and black stay as they are).
inline bool g_account_tint = false;

static inline uint16_t account_tint(uint16_t c) {
    return g_account_tint ? (uint16_t)(((c & 0x1F) << 11) | (c & 0x07E0) | (c >> 11)) : c;
}
