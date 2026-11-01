#pragma once
#include <stdbool.h>
#include <stdint.h>

// One finger's press → release, classified on release (clawdmeter2). Taps
// used to reach the director on the press; now every touch goes through
// here so a swipe between accounts isn't also a tap. Pure (test_gesture).

enum GestureKind {
    GESTURE_NONE,          // still down, a hold, or a drag that is neither
    GESTURE_TAP,           // short press that barely moved
    GESTURE_SWIPE_LEFT,    // finger moved right-to-left
    GESTURE_SWIPE_RIGHT,
};

struct Gesture {
    GestureKind kind;
    int16_t x, y;          // where the press started
};

struct GestureState {
    bool     down;
    int16_t  x0, y0, x, y;
    uint32_t t0;
};

#define GESTURE_TAP_SLOP_PX   24    // a tap may wander this far
#define GESTURE_TAP_MAX_MS    700   // longer is a hold (the pairing hint's hold)
#define GESTURE_SWIPE_MIN_PX  70    // horizontal travel for a swipe
#define GESTURE_SWIPE_MAX_MS  900

// Feed every touch read. Returns a gesture on the release edge.
Gesture gesture_step(GestureState* s, bool pressed, int16_t x, int16_t y, uint32_t now_ms);
