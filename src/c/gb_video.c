#include "gb_video.h"

#include <string.h>

static uint8_t s_fb[PB_GB_FRAME_BYTES];
static PbVideoScale s_scale = PB_VIDEO_SCALE_1X;

static const uint8_t SHADE_MASK[4] = {0xC0, 0x30, 0x0C, 0x03};
static const uint8_t SHADE_SHIFT[4] = {6, 4, 2, 0};

void pb_video_init(void) {
  pb_video_clear(0);
  s_scale = PB_VIDEO_SCALE_1X;
}

void pb_video_clear(uint8_t shade) {
  shade &= 3;
  uint8_t packed = (uint8_t)((shade << 6) | (shade << 4) | (shade << 2) | shade);
  memset(s_fb, packed, sizeof(s_fb));
}

static void prv_set_pixel(uint8_t x, uint8_t y, uint8_t shade) {
  uint16_t idx = (uint16_t)y * PB_GB_LCD_W + x;
  uint16_t byte = idx >> 2;
  uint8_t slot = idx & 3;
  s_fb[byte] = (uint8_t)((s_fb[byte] & ~SHADE_MASK[slot]) |
                         ((shade & 3) << SHADE_SHIFT[slot]));
}

uint8_t pb_video_get_pixel(uint8_t x, uint8_t y) {
  uint16_t idx = (uint16_t)y * PB_GB_LCD_W + x;
  uint16_t byte = idx >> 2;
  uint8_t slot = idx & 3;
  return (uint8_t)((s_fb[byte] & SHADE_MASK[slot]) >> SHADE_SHIFT[slot]);
}

void pb_video_draw_line(const uint8_t *pixels, uint8_t y) {
  if (y >= PB_GB_LCD_H) {
    return;
  }
  for (uint8_t x = 0; x < PB_GB_LCD_W; x++) {
    prv_set_pixel(x, y, pixels[x] & 3);
  }
}

const uint8_t *pb_video_framebuffer(void) {
  return s_fb;
}

uint32_t pb_video_hash(void) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < sizeof(s_fb); i++) {
    h ^= s_fb[i];
    h *= 16777619u;
  }
  return h;
}

void pb_video_set_scale(PbVideoScale scale) {
  s_scale = scale;
}

PbVideoScale pb_video_scale(void) {
  return s_scale;
}

#ifndef PB_DESKTOP
static uint8_t prv_argb_for_shade(uint8_t shade) {
  static const uint8_t palette[4] = {
    GColorWhiteARGB8,
    GColorLightGrayARGB8,
    GColorDarkGrayARGB8,
    GColorBlackARGB8,
  };
  return palette[shade & 3];
}

static void prv_plot(GBitmap *fb, int16_t x, int16_t y, uint8_t argb) {
  if (y < 0 || y >= PBL_DISPLAY_HEIGHT) {
    return;
  }
  GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
  if (x >= row.min_x && x <= row.max_x) {
    row.data[x] = argb;
  }
}

void pb_video_render(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }

  if (s_scale == PB_VIDEO_SCALE_FULLSCREEN) {
    for (int16_t y = 0; y < bounds.size.h; y++) {
      uint8_t src_y = (uint8_t)((int32_t)y * PB_GB_LCD_H / bounds.size.h);
      for (int16_t x = 0; x < bounds.size.w; x++) {
        uint8_t src_x = (uint8_t)((int32_t)x * PB_GB_LCD_W / bounds.size.w);
        prv_plot(fb, bounds.origin.x + x, bounds.origin.y + y,
                 prv_argb_for_shade(pb_video_get_pixel(src_x, src_y)));
      }
    }
  } else {
    int16_t ox = bounds.origin.x + (bounds.size.w - PB_GB_LCD_W) / 2;
    int16_t oy = bounds.origin.y + (bounds.size.h - PB_GB_LCD_H) / 2;
    for (uint8_t y = 0; y < PB_GB_LCD_H; y++) {
      for (uint8_t x = 0; x < PB_GB_LCD_W; x++) {
        prv_plot(fb, ox + x, oy + y, prv_argb_for_shade(pb_video_get_pixel(x, y)));
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}
#endif

