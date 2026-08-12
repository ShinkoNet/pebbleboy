#ifndef PB_GB_CORE_H
#define PB_GB_CORE_H

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 1
#define PEANUT_FULL_GBC_SUPPORT 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_INTRINSICS 1
#define PGB_UNREACHABLE() __builtin_unreachable()
#define JOYPAD_A 0x01
#define JOYPAD_B 0x02
#define JOYPAD_SELECT 0x04
#define JOYPAD_START 0x08
#define JOYPAD_RIGHT 0x10
#define JOYPAD_LEFT 0x20
#define JOYPAD_UP 0x40
#define JOYPAD_DOWN 0x80
#define PEANUT_GB_HEADER_ONLY
#ifndef PB_DESKTOP
struct tm;
#endif
#include "peanut_gb.h"
#undef PEANUT_GB_HEADER_ONLY

#endif
