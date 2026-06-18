#include "gb_app.h"

#include <pebble.h>
#include <stdio.h>
#include <string.h>

#include "gb_audio.h"
#include "gb_cart.h"
#include "gb_core.h"
#include "gb_input.h"
#include "gb_phone.h"
#include "gb_video.h"

#define FRAME_MS 33
#define FRAMES_PER_TICK 2
#define PHONE_INFO_RETRY_MS 2000
#define MAX_LOCAL_SAVE_RAM (32u * 1024u)

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_timer;
static struct gb_s *s_gb;
static PbCart *s_cart;
static bool s_running;
static bool s_phone_offer_seen;
static uint8_t *s_cart_ram;
static size_t s_cart_ram_size;
static char s_status[80];
static uint32_t s_frames;
static uint64_t s_last_log_ms;
static uint32_t s_last_log_frame;
static uint64_t s_last_phone_info_request_ms;

static void prv_frame_timer_cb(void *data);
static void prv_schedule_frame_timer(uint32_t delay_ms);

static uint64_t prv_now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (uint64_t)sec * 1000 + ms;
}

static void prv_set_status(const char *status) {
  strncpy(s_status, status ? status : "", sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = '\0';
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static uint8_t prv_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  PbCart *cart = (PbCart *)gb->direct.priv;
  return pb_cart_read(cart, (uint32_t)addr);
}

static uint8_t prv_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  if (addr < s_cart_ram_size && s_cart_ram) {
    return s_cart_ram[addr];
  }
  return 0xFF;
}

static void prv_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t value) {
  (void)gb;
  if (addr < s_cart_ram_size && s_cart_ram) {
    s_cart_ram[addr] = value;
  }
}

static void prv_gb_error(struct gb_s *gb, const enum gb_error_e error, const uint16_t addr) {
  (void)gb;
  snprintf(s_status, sizeof(s_status), "Core error %d @ %04x", (int)error, addr);
  APP_LOG(APP_LOG_LEVEL_ERROR, "%s", s_status);
}

static void prv_lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
  (void)gb;
  pb_video_draw_line(pixels, (uint8_t)line);
}

static void prv_free_save_ram(void) {
  free(s_cart_ram);
  s_cart_ram = NULL;
  s_cart_ram_size = 0;
}

static const char *prv_init_error_name(enum gb_init_error_e err) {
  switch (err) {
    case GB_INIT_NO_ERROR:
      return "ok";
    case GB_INIT_CARTRIDGE_UNSUPPORTED:
      return "unsupported cartridge";
    case GB_INIT_INVALID_CHECKSUM:
      return "bad cartridge checksum";
    default:
      return "unknown init error";
  }
}

static bool prv_start_from_cart(const char *source_name) {
  s_running = false;
  prv_free_save_ram();
  pb_video_clear(0);

  if (!pb_cart_has_bank(s_cart, 0)) {
    pb_cart_ensure_bank(s_cart, 0);
    prv_set_status("Loading bank 0");
    return false;
  }

  enum gb_init_error_e err = gb_init(s_gb, prv_rom_read, prv_cart_ram_read,
                                     prv_cart_ram_write, prv_gb_error, s_cart);
  if (err != GB_INIT_NO_ERROR) {
    snprintf(s_status, sizeof(s_status), "%s", prv_init_error_name(err));
    APP_LOG(APP_LOG_LEVEL_ERROR, "gb_init failed: %s", s_status);
    layer_mark_dirty(s_canvas);
    return false;
  }

  gb_init_lcd(s_gb, prv_lcd_draw_line);
  s_gb->direct.frame_skip = true;

  size_t save_size = 0;
  if (gb_get_save_size_s(s_gb, &save_size) == 0 && save_size > 0) {
    if (s_cart->mode == PB_CART_MODE_PHONE) {
      APP_LOG(APP_LOG_LEVEL_WARNING,
              "phone save sync not implemented; running without %u bytes of SRAM",
              (unsigned)save_size);
    } else if (save_size <= MAX_LOCAL_SAVE_RAM) {
      s_cart_ram = malloc(save_size);
      if (s_cart_ram) {
        memset(s_cart_ram, 0xFF, save_size);
        s_cart_ram_size = save_size;
      }
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "save RAM too large for local heap: %u",
              (unsigned)save_size);
    }
  }

  char title[17];
  gb_get_rom_name(s_gb, title);
  snprintf(s_status, sizeof(s_status), "%s %s", title[0] ? title : "DMG ROM", source_name);
  APP_LOG(APP_LOG_LEVEL_INFO, "started %s, save=%u, heap free=%u used=%u",
          s_status, (unsigned)s_cart_ram_size, (unsigned)heap_bytes_free(),
          (unsigned)heap_bytes_used());
  s_frames = 0;
  s_last_log_frame = 0;
  s_last_log_ms = prv_now_ms();
  s_running = true;
  return true;
}

static void prv_request_phone_bank(uint16_t bank, void *context) {
  (void)context;
  gb_phone_request_bank(bank);
  snprintf(s_status, sizeof(s_status), "Loading bank %u", bank);
  layer_mark_dirty(s_canvas);
}

void pb_core_rom_bank_changed(struct gb_s *gb) {
  if (gb != s_gb || !s_cart || s_cart->mode != PB_CART_MODE_PHONE) {
    return;
  }
  pb_cart_ensure_bank(s_cart, gb->selected_rom_bank);
}

bool pb_core_should_pause(struct gb_s *gb) {
  return gb == s_gb && s_cart && pb_cart_paused(s_cart);
}

static void prv_phone_event(const PbPhoneEvent *event, void *context) {
  (void)context;
  switch (event->type) {
    case PB_PHONE_EVENT_INFO:
      s_phone_offer_seen = true;
      APP_LOG(APP_LOG_LEVEL_INFO, "switching from %s to phone ROM",
              s_status[0] ? s_status : "local ROM");
      s_running = false;
      prv_free_save_ram();
      APP_LOG(APP_LOG_LEVEL_INFO, "phone info title=%s size=%lu cart=%u",
              event->title[0] ? event->title : "DMG ROM", event->size,
              (unsigned)event->cart_type);
      if (pb_cart_init_phone(s_cart, event->size, prv_request_phone_bank, NULL)) {
        snprintf(s_status, sizeof(s_status), "Phone ROM %s",
                 event->title[0] ? event->title : "loading");
        pb_cart_ensure_bank(s_cart, 0);
      }
      layer_mark_dirty(s_canvas);
      break;
    case PB_PHONE_EVENT_BANK_READY:
      APP_LOG(APP_LOG_LEVEL_INFO, "phone bank %u ready size=%u",
              (unsigned)event->bank, (unsigned)event->size);
      if (!s_running && event->bank == 0) {
        prv_start_from_cart("phone");
      } else if (s_cart && s_cart->mode == PB_CART_MODE_PHONE && !pb_cart_paused(s_cart)) {
        prv_set_status("Resumed");
        prv_schedule_frame_timer(1);
      }
      break;
    case PB_PHONE_EVENT_ERROR:
      if (s_phone_offer_seen || (s_cart && s_cart->mode == PB_CART_MODE_PHONE)) {
        pb_cart_set_error(s_cart, event->status);
        prv_set_status(event->status);
      }
      break;
  }
}

static void prv_log_perf(void) {
  uint64_t now = prv_now_ms();
  if (now - s_last_log_ms < 2000) {
    return;
  }
  const PbCartStats *stats = pb_cart_stats(s_cart);
  uint32_t frame_delta = s_frames - s_last_log_frame;
  uint32_t ms_delta = (uint32_t)(now - s_last_log_ms);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "fps=%u cache h=%lu m=%lu loads=%lu req=%lu last_miss=%u last_load=%u heap free=%u used=%u",
          (unsigned)((frame_delta * 1000u) / (ms_delta ? ms_delta : 1)),
          stats->hits, stats->misses, stats->loads, stats->phone_requests,
          (unsigned)stats->last_miss_bank, (unsigned)stats->last_load_bank,
          (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());
  s_last_log_ms = now;
  s_last_log_frame = s_frames;
}

static void prv_maybe_request_phone_info(uint64_t now) {
  if (s_phone_offer_seen || (s_cart && s_cart->mode == PB_CART_MODE_PHONE)) {
    return;
  }
  if (now - s_last_phone_info_request_ms < PHONE_INFO_RETRY_MS) {
    return;
  }
  gb_phone_request_info();
  s_last_phone_info_request_ms = now;
}

static void prv_schedule_frame_timer(uint32_t delay_ms) {
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
  s_timer = app_timer_register(delay_ms, prv_frame_timer_cb, NULL);
}

static bool prv_run_one_frame(void) {
  s_gb->direct.joypad = pb_input_joypad();
  if (!pb_cart_ensure_bank(s_cart, s_gb->selected_rom_bank) || pb_cart_paused(s_cart)) {
    return false;
  }
  gb_run_frame(s_gb);
  if (pb_cart_paused(s_cart)) {
    return false;
  }
  s_frames++;
  return true;
}

static void prv_frame_timer_cb(void *data) {
  (void)data;
  s_timer = NULL;
  prv_schedule_frame_timer(FRAME_MS);
  prv_maybe_request_phone_info(prv_now_ms());

  if (s_running && !pb_cart_paused(s_cart)) {
    for (int i = 0; i < FRAMES_PER_TICK; i++) {
      if (!prv_run_one_frame()) {
        break;
      }
    }
    pb_audio_pump();
    prv_log_perf();
  }

  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  pb_video_render(ctx, bounds);

  if (!s_running || !s_cart || pb_cart_paused(s_cart)) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_status, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(4, PBL_DISPLAY_HEIGHT - 34, PBL_DISPLAY_WIDTH - 8, 30),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void prv_raw_down_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  pb_input_release_touch();
  pb_input_press((uint8_t)(uintptr_t)context);
}

static void prv_raw_up_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  pb_input_release((uint8_t)(uintptr_t)context);
}

static void prv_back_release_cb(void *data) {
  (void)data;
  pb_input_release(JOYPAD_SELECT);
}

static void prv_back_click_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  (void)context;
  pb_input_release_touch();
  pb_input_press(JOYPAD_SELECT);
  app_timer_register(80, prv_back_release_cb, NULL);
}

static void prv_click_config_provider(void *context) {
  (void)context;
  window_raw_click_subscribe(BUTTON_ID_SELECT, prv_raw_down_handler, prv_raw_up_handler,
                             (void *)(uintptr_t)JOYPAD_A);
  window_raw_click_subscribe(BUTTON_ID_DOWN, prv_raw_down_handler, prv_raw_up_handler,
                             (void *)(uintptr_t)JOYPAD_B);
  window_raw_click_subscribe(BUTTON_ID_UP, prv_raw_down_handler, prv_raw_up_handler,
                             (void *)(uintptr_t)JOYPAD_START);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
}

#ifdef PBL_TOUCH
static AppTimer *s_touch_watchdog;

static void prv_touch_release(void) {
  if (s_touch_watchdog) {
    app_timer_cancel(s_touch_watchdog);
    s_touch_watchdog = NULL;
  }
  pb_input_release_touch();
}

static void prv_touch_watchdog_cb(void *data) {
  (void)data;
  s_touch_watchdog = NULL;
  prv_touch_release();
}

static void prv_touch_handler(const TouchEvent *event, void *context) {
  (void)context;
  if (event->type == TouchEvent_Liftoff) {
    prv_touch_release();
    return;
  }

  pb_input_touch_at(event->x, event->y, PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT);
  if (s_touch_watchdog) {
    app_timer_cancel(s_touch_watchdog);
  }
  s_touch_watchdog = app_timer_register(3000, prv_touch_watchdog_cb, NULL);
}
#endif

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas);

  pb_video_init();
  pb_input_init();
  pb_audio_init();
  s_cart = malloc(sizeof(*s_cart));
  s_gb = malloc(sizeof(*s_gb));
  if (!s_cart || !s_gb) {
    prv_set_status("Out of memory");
    return;
  }
  pb_cart_init_empty(s_cart);
  s_phone_offer_seen = false;
  s_last_phone_info_request_ms = 0;

  gb_phone_init(s_cart, prv_phone_event, NULL);

  if (pb_cart_init_resource(s_cart, RESOURCE_ID_TETRIS_ROM)) {
    prv_start_from_cart("local");
  } else {
    prv_set_status(s_cart->error[0] ? s_cart->error : "No local ROM");
  }

  gb_phone_request_info();
  s_last_phone_info_request_ms = prv_now_ms();

#ifdef PBL_TOUCH
  touch_service_subscribe(prv_touch_handler, NULL);
#endif

  prv_schedule_frame_timer(FRAME_MS);
}

static void prv_window_unload(Window *window) {
#ifdef PBL_TOUCH
  touch_service_unsubscribe();
  prv_touch_release();
#endif
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  gb_phone_deinit();
  pb_audio_deinit();
  prv_free_save_ram();
  free(s_cart);
  free(s_gb);
  s_cart = NULL;
  s_gb = NULL;
  layer_destroy(s_canvas);
  s_canvas = NULL;
  window_destroy(window);
  s_window = NULL;
}

void gb_app_push(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
