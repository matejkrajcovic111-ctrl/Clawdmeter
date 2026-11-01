#include "../../hal/touch_hal.h"
#include "board.h"
#include "../../hal/imu_hal.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>
#include <driver/gpio.h>
#include <soc/gpio_struct.h>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

// No touch ISR. attachInterrupt installs the GPIO ISR service, and on the
// dual-core S3 ESP-IDF always does that allocation inside the IPC task (1 KB
// stack, end-of-stack watchpoint on). An interrupt nesting there (USB traffic
// right after a flash) overflowed it: "Unhandled debug exception" boot loops.
// The GPIO block latches the falling edge in its status register whether or
// not a CPU interrupt is enabled, so the touch read polls and clears that bit.
static inline bool int_edge_latched(void) {
    const uint32_t m = 1u << TP_INT;   // TP_INT < 32: the low status register
    if (!(GPIO.status & m)) return false;
    GPIO.status_w1tc = m;
    return true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    gpio_intr_disable((gpio_num_t)TP_INT);             // never routed to a CPU
    gpio_set_intr_type((gpio_num_t)TP_INT, GPIO_INTR_NEGEDGE);
    GPIO.status_w1tc = 1u << TP_INT;
    touch_data_ready = true;                           // one read now: no stale state
    Serial.println("Touch init OK");
}

// display.cpp rotates LVGL's strips in software (rotate_strip), but the
// CST9220 keeps reporting in the panel's unrotated frame. Map the point back
// through the inverse of rotate_strip so hit-tests line up in every quadrant.
static void to_display_frame(uint16_t* x, uint16_t* y) {
    const uint16_t N = LCD_WIDTH - 1;   // square panel
    const uint16_t tx = *x > N ? N : *x, ty = *y > N ? N : *y;
    switch (imu_hal_rotation_quadrant()) {
    // MEASURED on hardware with corner taps (2026-10-30, quadrants 0/2/3;
    // quadrant 1 measured earlier). The touch frame sits a quarter turn off
    // the panel, so every quadrant is the inverse rotation plus that turn:
    // quadrant 3 needs no change at all and quadrant 0 needs a quarter turn.
    case 0: *x = ty;     *y = N - tx; break;
    case 1: *x = N - tx; *y = N - ty; break;
    case 2: *x = N - ty; *y = tx;     break;
    default: break;   // quadrant 3: the touch frame already matches
    }
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready || int_edge_latched()) {
        touch_data_ready = false;
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    to_display_frame(x, y);
    *pressed = touch_pressed;
    // TEMP touch calibration log: raw panel point + mapped point on each press.
    static bool was_pressed = false;
    if (touch_pressed && !was_pressed)
        Serial.printf("touchcal: rot=%u raw=%u,%u mapped=%u,%u\n",
                      (unsigned)imu_hal_rotation_quadrant(), (unsigned)touch_x, (unsigned)touch_y,
                      (unsigned)*x, (unsigned)*y);
    was_pressed = touch_pressed;
}
