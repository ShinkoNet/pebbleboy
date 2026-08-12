#include "gb_video.h"

#include <stdlib.h>
#include <string.h>

#define PB_GB_ROW_BYTES ((PB_GB_LCD_W * 3) / 4)
#define PB_OPAQUE_ARGB8 0xC0u

static uint8_t *s_fb;
static PbVideoScale s_scale = PB_VIDEO_SCALE_1X;
static bool s_changed;

/* Pebble's RGB channels are two bits wide. Keep the familiar four-shade DMG
 * ramp while using exactly the same packed representation as CGB pixels. */
static const uint8_t DMG_PALETTE[4] = {0x3F, 0x2A, 0x15, 0x00};

static void prv_pack_four(uint8_t *dst, uint8_t c0, uint8_t c1,
                          uint8_t c2, uint8_t c3) {
  dst[0] = (uint8_t)((c0 << 2) | (c1 >> 4));
  dst[1] = (uint8_t)((c1 << 4) | (c2 >> 2));
  dst[2] = (uint8_t)((c2 << 6) | c3);
}

static uint8_t prv_unpack(const uint8_t *row, uint8_t x) {
  const uint8_t *src = row + (uint16_t)(x >> 2) * 3u;
  switch (x & 3u) {
    case 0:
      return src[0] >> 2;
    case 1:
      return (uint8_t)(((src[0] & 3u) << 4) | (src[1] >> 4));
    case 2:
      return (uint8_t)(((src[1] & 15u) << 2) | (src[2] >> 6));
    default:
      return src[2] & 63u;
  }
}

static uint8_t prv_cgb_to_pebble(uint16_t colour) {
  /* pico-peanutGB's fixed palette stores RGB555 with red in bits 14:10. */
  return (uint8_t)((((colour >> 13) & 3u) << 4) |
                   (((colour >> 8) & 3u) << 2) |
                   ((colour >> 3) & 3u));
}

bool pb_video_init(void) {
  if (!s_fb) {
    s_fb = malloc(PB_GB_FRAME_BYTES);
  }
  if (!s_fb) {
    return false;
  }
  pb_video_clear(0);
  s_scale = PB_VIDEO_SCALE_1X;
  return true;
}

void pb_video_deinit(void) {
  free(s_fb);
  s_fb = NULL;
  s_changed = false;
}

void pb_video_clear(uint8_t shade) {
  if (!s_fb) {
    return;
  }
  uint8_t colour = DMG_PALETTE[shade & 3u];
  uint8_t packed[3];
  prv_pack_four(packed, colour, colour, colour, colour);
  for (uint32_t i = 0; i < PB_GB_FRAME_BYTES; i += 3) {
    memcpy(s_fb + i, packed, sizeof(packed));
  }
  s_changed = true;
}

uint8_t pb_video_get_pixel(uint8_t x, uint8_t y) {
  if (!s_fb || x >= PB_GB_LCD_W || y >= PB_GB_LCD_H) {
    return 0;
  }
  return prv_unpack(s_fb + (uint16_t)y * PB_GB_ROW_BYTES, x);
}

static void prv_draw_line(const uint8_t *pixels, const uint16_t *cgb_palette, uint8_t y) {
  if (!s_fb || y >= PB_GB_LCD_H) {
    return;
  }

  uint8_t *dst = s_fb + (uint16_t)y * PB_GB_ROW_BYTES;
  uint8_t difference = 0;
  for (uint8_t x = 0; x < PB_GB_LCD_W; x += 4) {
    uint8_t colours[4];
    for (uint8_t i = 0; i < 4; i++) {
      colours[i] = cgb_palette
                       ? prv_cgb_to_pebble(cgb_palette[pixels[x + i] & 63u])
                       : DMG_PALETTE[pixels[x + i] & 3u];
    }
    uint8_t packed[3];
    prv_pack_four(packed, colours[0], colours[1], colours[2], colours[3]);
    difference |= (uint8_t)(dst[0] ^ packed[0]);
    difference |= (uint8_t)(dst[1] ^ packed[1]);
    difference |= (uint8_t)(dst[2] ^ packed[2]);
    memcpy(dst, packed, sizeof(packed));
    dst += 3;
  }
  s_changed |= difference != 0;
}

void pb_video_draw_line(const uint8_t *pixels, uint8_t y) {
  prv_draw_line(pixels, NULL, y);
}

void pb_video_draw_line_cgb(const uint8_t *pixels, const uint16_t *palette, uint8_t y) {
  prv_draw_line(pixels, palette, y);
}

const uint8_t *pb_video_framebuffer(void) {
  return s_fb;
}

uint32_t pb_video_hash(void) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; s_fb && i < PB_GB_FRAME_BYTES; i++) {
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
static uint8_t prv_argb8(const uint8_t *row, uint8_t x) {
  return (uint8_t)(PB_OPAQUE_ARGB8 | prv_unpack(row, x));
}

void pb_video_render(GContext *ctx, GRect bounds) {
  if (!s_fb) {
    return;
  }
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
      const uint8_t *src = s_fb + (uint16_t)src_y * PB_GB_ROW_BYTES;
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, dst_y);
      int16_t first = ox > row.min_x ? ox : row.min_x;
      int16_t last = ox + out_w - 1 < row.max_x ? ox + out_w - 1 : row.max_x;
      for (int16_t dst_x = first; dst_x <= last; dst_x++) {
        row.data[dst_x] = prv_argb8(src, x_map[dst_x - ox]);
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
      const uint8_t *src = s_fb + (uint16_t)y * PB_GB_ROW_BYTES;
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, dst_y);
      int16_t first = ox > row.min_x ? ox : row.min_x;
      int16_t last = ox + PB_GB_LCD_W - 1 < row.max_x
                         ? ox + PB_GB_LCD_W - 1
                         : row.max_x;
      for (int16_t dst_x = first; dst_x <= last; dst_x++) {
        row.data[dst_x] = prv_argb8(src, (uint8_t)(dst_x - ox));
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}
#endif
