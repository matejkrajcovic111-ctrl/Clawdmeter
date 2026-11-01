#include "gesture.h"

static int16_t iabs(int v) { return (int16_t)(v < 0 ? -v : v); }

Gesture gesture_step(GestureState* s, bool pressed, int16_t x, int16_t y, uint32_t now_ms) {
    Gesture g = { GESTURE_NONE, 0, 0 };
    if (pressed) {
        if (!s->down) {
            s->down = true;
            s->x0 = s->x = x;
            s->y0 = s->y = y;
            s->t0 = now_ms;
        } else {
            s->x = x;
            s->y = y;
        }
        return g;
    }
    if (!s->down) return g;
    s->down = false;
    g.x = s->x0;
    g.y = s->y0;
    const int dx = s->x - s->x0, dy = s->y - s->y0;
    const uint32_t held = now_ms - s->t0;
    if (iabs(dx) >= GESTURE_SWIPE_MIN_PX && iabs(dx) > 2 * iabs(dy) && held <= GESTURE_SWIPE_MAX_MS) {
        g.kind = dx < 0 ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT;
    } else if (iabs(dx) <= GESTURE_TAP_SLOP_PX && iabs(dy) <= GESTURE_TAP_SLOP_PX && held <= GESTURE_TAP_MAX_MS) {
        g.kind = GESTURE_TAP;
    }
    return g;
}
