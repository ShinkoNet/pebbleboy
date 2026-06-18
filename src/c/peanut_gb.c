struct gb_s;
#include <stdbool.h>

#include "gb_audio.h"

void pb_core_rom_bank_changed(struct gb_s *gb);
bool pb_core_should_pause(struct gb_s *gb);

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#define PEANUT_GB_USE_INTRINSICS 1
#define PGB_UNREACHABLE() __builtin_unreachable()
#define PEANUT_GB_ROM_BANK_CHANGED(gb) pb_core_rom_bank_changed(gb)
#define PEANUT_GB_SHOULD_PAUSE(gb) pb_core_should_pause(gb)
#include "peanut_gb.h"
