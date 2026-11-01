#pragma once
#include <stdbool.h>

// clawdmeter2: "This device", the page left of account 1 (swipe left from
// #1, swipe right to go back). This board's own vitals laid out like the
// /usage screens (detail.cpp): processor, memory, PSRAM, app flash,
// temperature, battery, Bluetooth, uptime, brightness and firmware.
// A full-screen overlay on lv_layer_top(); refreshes once a second while up.
void sysmon_init(void);
void sysmon_show(void);
void sysmon_show_now(void);        // no slide (under the swipe portal)
void sysmon_hide(bool animated);   // animated: slides off to the right
bool sysmon_visible(void);
void sysmon_tick(void);
