#include "gb_video.h"

#include <string.h>

static uint8_t s_fb[PB_GB_FRAME_BYTES];
static PbVideoScale s_scale = PB_VIDEO_SCALE_1X;
static bool s_changed;

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
  s_changed = true;
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

  uint8_t *dst = s_fb + (uint16_t)y * (PB_GB_LCD_W / 4);
  uint8_t difference = 0;
  for (uint8_t x = 0; x < PB_GB_LCD_W; x += 4) {
    uint8_t packed = (uint8_t)(((pixels[x] & 3) << 6) |
                               ((pixels[x + 1] & 3) << 4) |
                               ((pixels[x + 2] & 3) << 2) |
                               (pixels[x + 3] & 3));
    difference |= (uint8_t)(*dst ^ packed);
    *dst++ = packed;
  }
  s_changed |= difference != 0;
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

bool pb_video_take_changed(void) {
  bool changed = s_changed;
  s_changed = false;
  return changed;
}

void pb_video_set_scale(PbVideoScale scale) {
  s_scale = scale;
}

PbVideoScale pb_video_scale(void) {
  return s_scale;
}

#ifndef PB_DESKTOP
static const uint8_t PALETTE[4] = {
  GColorWhiteARGB8,
  GColorLightGrayARGB8,
  GColorDarkGrayARGB8,
  GColorBlackARGB8,
};

static uint32_t s_palette_lut[256];
static bool s_palette_lut_initialized;

static void prv_init_palette_lut(void) {
  if (s_palette_lut_initialized) {
    return;
  }
  for (uint16_t packed = 0; packed < 256; packed++) {
    uint32_t expanded = 0;
    for (uint8_t slot = 0; slot < 4; slot++) {
      uint8_t shade = (uint8_t)((packed >> SHADE_SHIFT[slot]) & 3);
      expanded |= (uint32_t)PALETTE[shade] << (slot * 8);
    }
    s_palette_lut[packed] = expanded;
  }
  s_palette_lut_initialized = true;
}

static uint8_t prv_packed_shade(const uint8_t *row, uint8_t x) {
  uint8_t packed = row[x >> 2];
  return (uint8_t)((packed >> SHADE_SHIFT[x & 3]) & 3);
}

void pb_video_render(GContext *ctx, GRect bounds) {
  prv_init_palette_lut();
  if (s_scale != PB_VIDEO_SCALE_1X) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }

  if (s_scale == PB_VIDEO_SCALE_FULLSCREEN || s_scale == PB_VIDEO_SCALE_ASPECT_FIT) {
    int16_t out_w = bounds.size.w;
    int16_t out_h = bounds.size.h;
    int16_t ox = bounds.origin.x;
    int16_t oy = bounds.origin.y;

    if (s_scale == PB_VIDEO_SCALE_ASPECT_FIT) {
      int32_t width_scaled_h = (int32_t)bounds.size.w * PB_GB_LCD_H / PB_GB_LCD_W;
      int32_t height_scaled_w = (int32_t)bounds.size.h * PB_GB_LCD_W / PB_GB_LCD_H;
      if (width_scaled_h <= bounds.size.h) {
        out_h = (int16_t)width_scaled_h;
        oy = bounds.origin.y + (bounds.size.h - out_h) / 2;
      } else {
        out_w = (int16_t)height_scaled_w;
        ox = bounds.origin.x + (bounds.size.w - out_w) / 2;
      }
    }

    uint8_t x_map[PBL_DISPLAY_WIDTH];
    for (int16_t x = 0; x < out_w; x++) {
      x_map[x] = (uint8_t)((int32_t)x * PB_GB_LCD_W / out_w);
    }

    for (int16_t y = 0; y < out_h; y++) {
      int16_t dst_y = oy + y;
      if (dst_y < 0 || dst_y >= PBL_DISPLAY_HEIGHT) {
        continue;
      }
      uint8_t src_y = (uint8_t)((int32_t)y * PB_GB_LCD_H / out_h);
      const uint8_t *src = s_fb + (uint16_t)src_y * (PB_GB_LCD_W / 4);
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, dst_y);
      int16_t first = ox > row.min_x ? ox : row.min_x;
      int16_t last = ox + out_w - 1 < row.max_x ? ox + out_w - 1 : row.max_x;
      for (int16_t dst_x = first; dst_x <= last; dst_x++) {
        uint8_t src_x = x_map[dst_x - ox];
        row.data[dst_x] = PALETTE[prv_packed_shade(src, src_x)];
      }
    }
  } else {
    int16_t ox = bounds.origin.x + (bounds.size.w - PB_GB_LCD_W) / 2;
    int16_t oy = bounds.origin.y + (bounds.size.h - PB_GB_LCD_H) / 2;
    for (uint8_t y = 0; y < PB_GB_LCD_H; y++) {
      int16_t dst_y = oy + y;
      if (dst_y < 0 || dst_y >= PBL_DISPLAY_HEIGHT) {
        continue;
      }
      const uint8_t *src = s_fb + (uint16_t)y * (PB_GB_LCD_W / 4);
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, dst_y);
      int16_t first = ox > row.min_x ? ox : row.min_x;
      int16_t last = ox + PB_GB_LCD_W - 1 < row.max_x
                         ? ox + PB_GB_LCD_W - 1
                         : row.max_x;
      if (first == ox && last == ox + PB_GB_LCD_W - 1) {
        uint8_t *dst = row.data + ox;
        for (uint8_t i = 0; i < PB_GB_LCD_W / 4; i++) {
          uint32_t expanded = s_palette_lut[src[i]];
          memcpy(dst + i * 4, &expanded, sizeof(expanded));
        }
        continue;
      }
      for (int16_t dst_x = first; dst_x <= last; dst_x++) {
        uint8_t src_x = (uint8_t)(dst_x - ox);
        row.data[dst_x] = PALETTE[prv_packed_shade(src, src_x)];
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}
#endif
