#include "gb_input.h"

#include <stdlib.h>

static uint8_t s_buttons = 0xFF;
static uint8_t s_touch_mask;

void pb_input_init(void) {
  s_buttons = 0xFF;
  s_touch_mask = 0;
}

void pb_input_press(uint8_t mask) {
  s_buttons &= (uint8_t)~mask;
}

void pb_input_release(uint8_t mask) {
  s_buttons |= mask;
}

void pb_input_release_touch(void) {
  if (s_touch_mask) {
    pb_input_release(s_touch_mask);
    s_touch_mask = 0;
  }
}

void pb_input_touch_at(int16_t x, int16_t y, int16_t w, int16_t h) {
  int16_t cx = w / 2;
  int16_t cy = h / 2;
  int16_t dx = x - cx;
  int16_t dy = y - cy;
  uint8_t mask = (abs(dx) > abs(dy))
      ? (dx < 0 ? JOYPAD_LEFT : JOYPAD_RIGHT)
      : (dy < 0 ? JOYPAD_UP : JOYPAD_DOWN);

  if (mask != s_touch_mask) {
    pb_input_release_touch();
    s_touch_mask = mask;
    pb_input_press(mask);
  }
}

uint8_t pb_input_joypad(void) {
  return s_buttons;
}

