#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

// Permission card: Claude Code asks to use a tool, the device shows a card on
// lv_layer_top() with Deny (left) and Allow (right) and a line that shrinks
// over the timeout. Everything is created once in perm_init(); show/hide only
// restyle. Taps are routed by the caller through perm_hit().
void perm_init(lv_obj_t* parent);          // builds it hidden, once after ui_init (NULL = layer top)
void perm_show(uint32_t id, const char* tool, const char* text, uint32_t timeout_ms);  // replaces any shown one
void perm_hide(uint32_t id);               // withdraws it if that id is shown (id 0 = any)
bool perm_visible(void);
// A tap while visible, in screen px: 1 = Allow hit, 2 = Deny hit, 0 = elsewhere (ignore).
int  perm_hit(int x, int y);
uint32_t perm_current_id(void);
void perm_tick(void);                      // every loop: times out, ends the press flash
bool perm_take_answer(uint32_t* id, bool* allow);   // set by perm_answer; consumed once
void perm_answer(bool allow);              // records the answer, flashes the button, then hides
