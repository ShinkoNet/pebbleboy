#ifndef PB_GB_CORE_H
#define PB_GB_CORE_H

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#define PEANUT_GB_USE_INTRINSICS 1
#define PGB_UNREACHABLE() __builtin_unreachable()
#define PEANUT_GB_HEADER_ONLY
#ifndef PB_DESKTOP
struct tm;
#endif
#include "peanut_gb.h"
#undef PEANUT_GB_HEADER_ONLY

#endif
