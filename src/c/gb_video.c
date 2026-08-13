#include "gb_video.h"

#define PB_OPAQUE_ARGB8 0xC0u

static PbVideoScale s_scale = PB_VIDEO_SCALE_1X;

/* Pebble's RGB channels are two bits wide. Keep the familiar four-shade DMG
 * ramp while using exactly the same representation as native CGB output. */
static const uint8_t DMG_PALETTE[4] = {0x3F, 0x2A, 0x15, 0x00};

static uint8_t prv_cgb_to_pebble(uint16_t colour) {
  /* pico-peanutGB's fixed palette stores RGB555 with red in bits 14:10. */
  return (uint8_t)((((colour >> 13) & 3u) << 4) |
                   (((colour >> 8) & 3u) << 2) |
                   ((colour >> 3) & 3u));
}

bool pb_video_init(void) {
  s_scale = PB_VIDEO_SCALE_1X;
  return true;
}

void pb_video_deinit(void) {
}

void pb_video_clear(uint8_t shade) {
  (void)shade;
}

void pb_video_set_scale(PbVideoScale scale) {
  s_scale = scale;
}

PbVideoScale pb_video_scale(void) {
  return s_scale;
}

#ifndef PB_DESKTOP
static GBitmap *s_target;
static int16_t s_output_x;
static int16_t s_output_y;
static int16_t s_output_w;
static int16_t s_output_h;

bool pb_video_begin_frame(GContext *ctx, GRect bounds) {
  if (s_target) {
    return false;
  }

  s_output_x = bounds.origin.x;
  s_output_y = bounds.origin.y;
  s_output_w = bounds.size.w;
  s_output_h = bounds.size.h;
  if (s_scale == PB_VIDEO_SCALE_1X) {
    s_output_x += (bounds.size.w - PB_GB_LCD_W) / 2;
    s_output_y += (bounds.size.h - PB_GB_LCD_H) / 2;
    s_output_w = PB_GB_LCD_W;
    s_output_h = PB_GB_LCD_H;
  } else {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    if (s_scale == PB_VIDEO_SCALE_ASPECT_FIT) {
      int32_t width_scaled_h = (int32_t)bounds.size.w * PB_GB_LCD_H / PB_GB_LCD_W;
      int32_t height_scaled_w = (int32_t)bounds.size.h * PB_GB_LCD_W / PB_GB_LCD_H;
      if (width_scaled_h <= bounds.size.h) {
        s_output_h = (int16_t)width_scaled_h;
        s_output_y += (bounds.size.h - s_output_h) / 2;
      } else {
        s_output_w = (int16_t)height_scaled_w;
        s_output_x += (bounds.size.w - s_output_w) / 2;
      }
    }
  }

  s_target = graphics_capture_frame_buffer(ctx);
  return s_target != NULL;
}

void pb_video_end_frame(GContext *ctx) {
  if (!s_target) {
    return;
  }
  graphics_release_frame_buffer(ctx, s_target);
  s_target = NULL;
}

static __attribute__((optimize("O3")))
void prv_draw_line(const uint8_t *pixels, const uint16_t *cgb_palette,
                   uint8_t source_y) {
  if (!s_target || source_y >= PB_GB_LCD_H) {
    return;
  }

  /* These are exactly the destination rows for which
   * floor(relative_y * 144 / output_h) == source_y. */
  int16_t first_y = (int16_t)(((uint32_t)source_y * s_output_h +
                               PB_GB_LCD_H - 1u) /
                              PB_GB_LCD_H);
  int16_t end_y = (int16_t)(((uint32_t)(source_y + 1u) * s_output_h +
                             PB_GB_LCD_H - 1u) /
                            PB_GB_LCD_H);
  for (int16_t relative_y = first_y; relative_y < end_y; relative_y++) {
    int16_t destination_y = s_output_y + relative_y;
    if (destination_y < 0 || destination_y >= PBL_DISPLAY_HEIGHT) {
      continue;
    }

    GBitmapDataRowInfo row = gbitmap_get_data_row_info(s_target, destination_y);
    int16_t first_x = s_output_x > row.min_x ? s_output_x : row.min_x;
    int16_t last_x = s_output_x + s_output_w - 1 < row.max_x
                         ? s_output_x + s_output_w - 1
                         : row.max_x;
    if (first_x > last_x) {
      continue;
    }

    int16_t relative_x = first_x - s_output_x;
    uint32_t scaled_x = (uint32_t)relative_x * PB_GB_LCD_W;
    uint16_t source_x = (uint16_t)(scaled_x / s_output_w);
    uint16_t remainder = (uint16_t)(scaled_x % s_output_w);
    for (int16_t destination_x = first_x; destination_x <= last_x;
         destination_x++) {
      uint8_t pixel = pixels[source_x];
      uint8_t colour = cgb_palette
                           ? prv_cgb_to_pebble(cgb_palette[pixel & 63u])
                           : DMG_PALETTE[pixel & 3u];
      row.data[destination_x] = (uint8_t)(PB_OPAQUE_ARGB8 | colour);

      remainder = (uint16_t)(remainder + PB_GB_LCD_W);
      while (remainder >= (uint16_t)s_output_w) {
        remainder = (uint16_t)(remainder - s_output_w);
        source_x++;
      }
    }
  }
}

void pb_video_draw_line(const uint8_t *pixels, uint8_t y) {
  prv_draw_line(pixels, NULL, y);
}

void pb_video_draw_line_cgb(const uint8_t *pixels, const uint16_t *palette,
                            uint8_t y) {
  prv_draw_line(pixels, palette, y);
}
#else
void pb_video_draw_line(const uint8_t *pixels, uint8_t y) {
  (void)pixels;
  (void)y;
}

void pb_video_draw_line_cgb(const uint8_t *pixels, const uint16_t *palette,
                            uint8_t y) {
  (void)pixels;
  (void)palette;
  (void)y;
}
#endif
