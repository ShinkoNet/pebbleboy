#ifndef PB_GB_VIDEO_H
#define PB_GB_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define PB_GB_LCD_W 160
#define PB_GB_LCD_H 144
typedef enum {
  PB_VIDEO_SCALE_1X = 0,
  PB_VIDEO_SCALE_FULLSCREEN,
  PB_VIDEO_SCALE_ASPECT_FIT,
} PbVideoScale;

bool pb_video_init(void);
void pb_video_deinit(void);
void pb_video_clear(uint8_t shade);
void pb_video_draw_line(const uint8_t *pixels, uint8_t y);
void pb_video_draw_line_cgb(const uint8_t *pixels, const uint16_t *palette, uint8_t y);
void pb_video_set_scale(PbVideoScale scale);
PbVideoScale pb_video_scale(void);

#ifndef PB_DESKTOP
/* Capture Pebble's native framebuffer while the emulator produces its visible
 * frame. Scanline callbacks write directly into it, avoiding a 17 KiB shadow
 * framebuffer in the app heap. */
bool pb_video_begin_frame(GContext *ctx, GRect bounds);
void pb_video_end_frame(GContext *ctx);
#endif

#endif
