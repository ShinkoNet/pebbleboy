#ifndef PB_GB_INPUT_H
#define PB_GB_INPUT_H

#include <stdint.h>

#include "gb_core.h"

void pb_input_init(void);
void pb_input_press(uint8_t mask);
void pb_input_release(uint8_t mask);
void pb_input_release_touch(void);
void pb_input_touch_at(int16_t x, int16_t y, int16_t w, int16_t h);
uint8_t pb_input_joypad(void);

#endif

