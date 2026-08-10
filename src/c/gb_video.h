#ifndef PB_GB_VIDEO_H
#define PB_GB_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define PB_GB_LCD_W 160
#define PB_GB_LCD_H 144
#define PB_GB_FRAME_BYTES ((PB_GB_LCD_W * PB_GB_LCD_H) / 4)

typedef enum {
  PB_VIDEO_SCALE_1X = 0,
  PB_VIDEO_SCALE_FULLSCREEN,
  PB_VIDEO_SCALE_ASPECT_FIT,
} PbVideoScale;

void pb_video_init(void);
void pb_video_clear(uint8_t shade);
void pb_video_draw_line(const uint8_t *pixels, uint8_t y);
uint8_t pb_video_get_pixel(uint8_t x, uint8_t y);
const uint8_t *pb_video_framebuffer(void);
uint32_t pb_video_hash(void);
bool pb_video_take_changed(void);
void pb_video_set_scale(PbVideoScale scale);
PbVideoScale pb_video_scale(void);

#ifndef PB_DESKTOP
void pb_video_render(GContext *ctx, GRect bounds);
#endif

#endif
