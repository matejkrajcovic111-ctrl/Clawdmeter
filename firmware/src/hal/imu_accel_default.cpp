#include "imu_hal.h"

// Boards without an accelerometer reading (or that don't expose one yet):
// no shake or tilt for the pet. A board's imu.cpp overrides this.
__attribute__((weak)) bool imu_hal_accel(float* x, float* y, float* z) {
    (void)x; (void)y; (void)z;
    return false;
}
