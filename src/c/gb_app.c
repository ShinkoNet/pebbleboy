#include "gb_app.h"

#include <pebble.h>
#include <stdio.h>
#include <string.h>

#include "gb_audio.h"
#include "gb_blob.h"
#include "gb_cart.h"
#include "gb_core.h"
#include "gb_input.h"
#include "gb_phone.h"
#include "gb_save.h"
#include "gb_store.h"
#include "gb_video.h"

#define FRAME_RATE_HZ 60u
#define FRAME_BASE_MS (1000u / FRAME_RATE_HZ)
#define FRAME_REMAINDER_MS (1000u % FRAME_RATE_HZ)
#define FRAME_CATCHUP_LIMIT_MS (FRAME_BASE_MS * 8u)
#define NORMAL_FRAMES_PER_TICK 1u
#define FAST_FRAMES_PER_TICK 2u
#define PHONE_INFO_RETRY_MS 2000
/* Persist storage is already chunked at 256 bytes. Paging SRAM at that same
 * granularity avoids holding a mostly cold 4 KiB bank in the app heap. */
#define LOCAL_CART_RAM_WINDOW_SIZE PB_SAVE_CHUNK_SIZE
#define ROM_LIBRARY_MAX 12u
#define ROM_SELECTOR_VISIBLE_ROWS 5u
#define PB_PREF_AUDIO 0x50424155u /* PBAU */
#define PB_PREF_SCALE 0x50425343u /* PBSC */
#define PB_RTC_MAGIC 0x50525443u /* PRTC */
#define PB_RTC_VERSION 1u

#if defined(__GNUC__)
#define PB_SIZE_OPT __attribute__((optimize("Os")))
#else
#define PB_SIZE_OPT
#endif

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_timer;
static struct gb_s *s_gb;
static PbCart *s_cart;
static bool s_running;
static bool s_in_focus = true;
static bool s_waiting_launch;
static bool s_settings_open;
static uint64_t s_auto_launch_deadline;
static bool s_fast_forward;
static bool s_phone_offer_seen;
static bool s_phone_library_seen;
static bool s_rom_selector;
static bool s_rom_selection_pending;
static uint8_t s_rom_count;
static uint8_t s_rom_selected;
static char s_rom_titles[ROM_LIBRARY_MAX][17];
static uint8_t *s_cart_ram;
static size_t s_cart_ram_size;
static size_t s_cart_ram_window_size;
static bool s_cart_ram_dirty;
static PbSave s_local_save;
static uint32_t s_save_revision;
static bool s_save_importing;
static uint32_t s_sync_token, s_sync_revision, s_sync_crc, s_sync_offset;
static uint64_t s_sync_activity;
static bool s_sync_active, s_sync_committed;
static uint64_t s_sync_import_time;
static uint64_t s_save_sync_deadline;
static uint8_t s_sync_rtc[PB_STORE_RTC_SIZE];
static uint64_t s_last_save_flush;

static bool s_rtc_active;
static uint16_t s_rtc_checksum;
static char s_status[80];
static uint32_t s_frames;
static uint64_t s_last_log_ms;
static uint32_t s_last_log_frame;
static uint64_t s_last_phone_info_request_ms;
static uint64_t s_next_frame_deadline_ms;
static uint8_t s_frame_deadline_remainder;
/* A compositor callback may advance one displayed frame between timer ticks.
 * The next timer subtracts that credit from its headless work, keeping game
 * and audio time fixed even when Pebble coalesces presentation requests. */
static bool s_draw_frame_pending;
static uint8_t s_draw_frame_credit;

#ifdef PEBBLEBOY_APP_BLOB
typedef struct {
  bool active;
  uint32_t size;
  uint32_t crc32;
  uint32_t offset;
  char title[17];
} PbRomInstall;

static PbRomInstall s_rom_install;
static uint32_t s_cached_rom_size;
static uint64_t s_library_deadline;
static bool s_cached_resume;
#endif

typedef struct {
  uint32_t cpu_ms;
  uint32_t audio_ms;
  uint32_t save_ms;
  uint32_t render_ms;
  uint32_t tick_ms;
  uint32_t max_tick_ms;
  uint32_t ticks;
  uint32_t renders;
  uint32_t presented;
  uint32_t unchanged;
  uint32_t catchups;
} PbPerfProfile;

static PbPerfProfile s_profile;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t rom_checksum;
  uint8_t registers[5];
  uint8_t reserved[3];
  uint32_t saved_at;
  uint32_t saved_at_inverse;
} PbRtcPersist;

_Static_assert(sizeof(PbRtcPersist) == PB_STORE_RTC_SIZE, "Backup RTC layout changed");

static void prv_frame_timer_cb(void *data);
static void prv_schedule_frame_timer(uint32_t delay_ms);
static void prv_update_canvas_frame(void);

static void prv_advance_frame_deadline(void) {
  s_next_frame_deadline_ms += FRAME_BASE_MS;
  s_frame_deadline_remainder =
      (uint8_t)(s_frame_deadline_remainder + FRAME_REMAINDER_MS);
  if (s_frame_deadline_remainder >= FRAME_RATE_HZ) {
    s_next_frame_deadline_ms++;
    s_frame_deadline_remainder =
        (uint8_t)(s_frame_deadline_remainder - FRAME_RATE_HZ);
  }
}

static void prv_reset_frame_deadline(uint64_t now_ms, bool immediate) {
  s_next_frame_deadline_ms = now_ms;
  s_frame_deadline_remainder = 0;
  if (!immediate) {
    prv_advance_frame_deadline();
  }
}

static void prv_set_rom_selector(bool selecting) {
  s_rom_selector = selecting;
  if (!s_window || !s_canvas) {
    return;
  }
  if (selecting) {
    layer_set_frame(s_canvas, layer_get_bounds(window_get_root_layer(s_window)));
    layer_mark_dirty(s_canvas);
  } else {
    prv_update_canvas_frame();
  }
}

static uint64_t prv_now_ms(void) {
  static uint64_t last_ms;
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  uint64_t now = (uint64_t)sec * 1000 + ms;
  if (now < last_ms && last_ms - now < 1000) {
    now += 1000;
  }
  if (now < last_ms) {
    return last_ms;
  }
  last_ms = now;
  return now;
}

static uint32_t prv_elapsed_ms(uint64_t start_ms) {
  uint64_t now = prv_now_ms();
  return now >= start_ms ? (uint32_t)(now - start_ms) : 0;
}

static void prv_set_status(const char *status) {
  strncpy(s_status, status ? status : "", sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = '\0';
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_update_canvas_frame(void) {
  if (!s_window || !s_canvas) {
    return;
  }
  GRect root_bounds = layer_get_bounds(window_get_root_layer(s_window));
  GRect frame = root_bounds;
  if (pb_video_scale() == PB_VIDEO_SCALE_1X) {
    frame = GRect(root_bounds.origin.x + (root_bounds.size.w - PB_GB_LCD_W) / 2,
                  root_bounds.origin.y + (root_bounds.size.h - PB_GB_LCD_H) / 2,
                  PB_GB_LCD_W, PB_GB_LCD_H);
  }
  layer_set_frame(s_canvas, frame);
  layer_mark_dirty(s_canvas);
}

static void prv_apply_phone_settings(const PbPhoneEvent *event) {
  PbVideoScale scale = event->video_scale <= PB_VIDEO_SCALE_ASPECT_FIT
                           ? (PbVideoScale)event->video_scale
                           : PB_VIDEO_SCALE_1X;
  pb_video_set_scale(scale);
  pb_audio_set_enabled(event->audio_enabled);
  persist_write_int(PB_PREF_SCALE, scale);
  persist_write_int(PB_PREF_AUDIO, event->audio_enabled ? 1 : 0);
  prv_update_canvas_frame();
}

static uint8_t prv_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  PbCart *cart = (PbCart *)gb->direct.priv;
  return pb_cart_read(cart, (uint32_t)addr);
}

static bool prv_select_local_save_addr(size_t addr) {
  if (addr >= s_cart_ram_size) {
    return false;
  }
  if (addr >= s_local_save.window_start &&
      addr - s_local_save.window_start < s_local_save.data_size) {
    return true;
  }

  if (!pb_save_select_window(&s_local_save, addr)) {
    prv_set_status("SRAM window failed");
    return false;
  }
  s_cart_ram_dirty = false;
  return true;
}

static uint8_t prv_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  if (addr < s_cart_ram_size) {
    if (!prv_select_local_save_addr(addr)) {
      return 0xFF;
    }
    return pb_save_read(&s_local_save, addr);
  }
  return 0xFF;
}

static void prv_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t value) {
  (void)gb;
  if (addr < s_cart_ram_size) {
    if (!prv_select_local_save_addr(addr)) {
      return;
    }
    if (pb_save_write(&s_local_save, addr, value)) {
      s_cart_ram_dirty = true;
    s_save_revision++;
    }
  }
}

static void prv_gb_error(struct gb_s *gb, const enum gb_error_e error, const uint16_t addr) {
  (void)gb;
  snprintf(s_status, sizeof(s_status), "Core error %d @ %04x", (int)error, addr);
  APP_LOG(APP_LOG_LEVEL_ERROR, "%s", s_status);
}

static void prv_lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
  if (gb->cgb.cgbMode) {
    pb_video_draw_line_cgb(pixels, gb->cgb.fixPalette, (uint8_t)line);
  } else {
    pb_video_draw_line(pixels, (uint8_t)line);
  }
}

static void prv_set_fast_forward(bool enabled) {
  if (s_fast_forward == enabled) {
    return;
  }
  s_fast_forward = enabled;
  if (s_gb) {
    s_gb->direct.frame_skip = false;
    s_gb->display.lcd_draw_line = prv_lcd_draw_line;
    s_gb->display.frame_skip_count = 0;
  }
  if (enabled) {
    pb_audio_suspend_stream();
  }
  prv_reset_frame_deadline(prv_now_ms(), true);
  APP_LOG(APP_LOG_LEVEL_INFO, "fast-forward %s", enabled ? "enabled" : "disabled");
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_free_save_ram(void) {
  free(s_cart_ram);
  s_cart_ram = NULL;
  memset(&s_local_save, 0, sizeof(s_local_save));
  s_cart_ram_size = 0;
  s_cart_ram_window_size = 0;
  s_cart_ram_dirty = false;
}

static uint16_t prv_rom_checksum(void) {
  uint16_t checksum = (uint16_t)((uint16_t)pb_cart_read(s_cart, 0x14E) << 8);
  return checksum | pb_cart_read(s_cart, 0x14F);
}

static bool prv_rtc_registers_valid(const uint8_t registers[5]) {
  return registers[0] < 60 && registers[1] < 60 && registers[2] < 24 &&
         (registers[4] & (uint8_t)~0xC1u) == 0;
}

static bool prv_flush_rtc(const char *reason) {
  if (!s_rtc_active || !s_gb || !s_gb->cart_has_rtc) {
    return true;
  }
  time_t now = time(NULL);
  uint32_t saved_at = now > 0 ? (uint32_t)now : 0;
  PbRtcPersist state = {
    .magic = PB_RTC_MAGIC,
    .version = PB_RTC_VERSION,
    .rom_checksum = s_rtc_checksum,
    .saved_at = saved_at,
    .saved_at_inverse = ~saved_at,
  };
  memcpy(state.registers, s_gb->cart_rtc, sizeof(state.registers));
  int written = pb_store_write_rtc(&state);
  APP_LOG(written == (int)sizeof(state) ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_ERROR,
          "RTC %s saved=%d time=%u:%02u:%02u day=%u flags=%02x",
          reason, written, (unsigned)state.registers[2],
          (unsigned)state.registers[1], (unsigned)state.registers[0],
          (unsigned)(state.registers[3] | ((state.registers[4] & 1u) << 8)),
          (unsigned)state.registers[4]);
  return written == (int)sizeof(state);
}

static void prv_init_rtc(void) {
  s_rtc_active = false;
  if (!s_gb->cart_has_rtc) {
    return;
  }

  s_rtc_checksum = prv_rom_checksum();
  s_rtc_active = true;
  PbRtcPersist state;
  int read = pb_store_read_rtc(&state);
  if (read != (int)sizeof(state) || state.magic != PB_RTC_MAGIC ||
      state.version != PB_RTC_VERSION || state.rom_checksum != s_rtc_checksum ||
      state.saved_at_inverse != ~state.saved_at ||
      !prv_rtc_registers_valid(state.registers)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "RTC initialized without persisted state checksum=%04x",
            (unsigned)s_rtc_checksum);
    return;
  }

  memcpy(s_gb->cart_rtc, state.registers, sizeof(s_gb->cart_rtc));
  time_t now = time(NULL);
  uint32_t elapsed = now > 0 && (uint32_t)now >= state.saved_at
                         ? (uint32_t)now - state.saved_at
                         : 0;
  gb_advance_rtc(s_gb, elapsed);
  memcpy(s_gb->cart_rtc_latched, s_gb->cart_rtc,
         sizeof(s_gb->cart_rtc_latched));
  s_gb->rtc_latched = false;
  s_gb->rtc_latch_last = false;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "RTC restored elapsed=%lu time=%u:%02u:%02u day=%u flags=%02x",
          (unsigned long)elapsed, (unsigned)s_gb->cart_rtc[2],
          (unsigned)s_gb->cart_rtc[1], (unsigned)s_gb->cart_rtc[0],
          (unsigned)(s_gb->cart_rtc[3] | ((s_gb->cart_rtc[4] & 1u) << 8)),
          (unsigned)s_gb->cart_rtc[4]);
}

static void prv_flush_local_save(const char *reason) {
  if (!s_cart_ram || !s_cart_ram_dirty) {
    return;
  }
  uint64_t started_ms = prv_now_ms();
  int flushed = pb_save_flush_all(&s_local_save);
  s_cart_ram_dirty = pb_save_dirty_count(&s_local_save) != 0;
  if (flushed < 0) {
    s_running = false;
    prv_set_status("Save write failed: storage full?");
  }
  APP_LOG(flushed < 0 ? APP_LOG_LEVEL_ERROR : APP_LOG_LEVEL_INFO,
          "local SRAM %s flush=%d dirty=%u writes=%u bytes=%lu elapsed=%lu",
          reason, flushed, (unsigned)pb_save_dirty_count(&s_local_save),
          (unsigned)s_local_save.write_ops, (unsigned long)s_local_save.write_bytes,
          (unsigned long)prv_elapsed_ms(started_ms));
}

static void prv_focus_handler(bool in_focus) {
  s_in_focus = in_focus;
  if (!in_focus) {
    s_draw_frame_pending = false;
    s_draw_frame_credit = 0;
    pb_audio_suspend_stream();
    prv_flush_local_save("focus-loss");
    prv_flush_rtc("focus-loss");
    return;
  }

  prv_reset_frame_deadline(prv_now_ms(), true);
  if (s_running) {
    prv_schedule_frame_timer(1);
  }
}

static bool prv_init_local_save(size_t save_size) {
  if (save_size == 0 || save_size > PB_SAVE_MAX_SIZE) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "local SRAM size unsupported: %u", (unsigned)save_size);
    prv_set_status("SRAM too large");
    return false;
  }

  size_t window_size = save_size < LOCAL_CART_RAM_WINDOW_SIZE
                           ? save_size
                           : LOCAL_CART_RAM_WINDOW_SIZE;
  s_cart_ram = malloc(window_size);
  if (!s_cart_ram) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "local SRAM allocation failed: %u/%u",
            (unsigned)window_size, (unsigned)save_size);
    prv_set_status("No SRAM heap");
    return false;
  }

  uint16_t rom_checksum = prv_rom_checksum();
  if (!pb_save_init_window(&s_local_save, s_cart_ram, save_size, window_size,
                           rom_checksum, pb_store_read,
                           pb_store_write, NULL)) {
    free(s_cart_ram);
    s_cart_ram = NULL;
    prv_set_status("SRAM init failed");
    return false;
  }

  s_cart_ram_size = save_size;
  s_cart_ram_window_size = window_size;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "local SRAM ready: %u/%u checksum=%04x restored=%u",
          (unsigned)window_size, (unsigned)save_size, (unsigned)rom_checksum,
          (unsigned)s_local_save.restored_chunks);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "local SRAM reads=%u bytes=%lu heap=%u/%u",
          (unsigned)s_local_save.read_ops, (unsigned long)s_local_save.read_bytes,
          (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());
  return true;
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
  prv_flush_rtc("ROM-change");
  s_rtc_active = false;
  s_running = false;
  s_fast_forward = false;
  s_draw_frame_pending = false;
  s_draw_frame_credit = 0;
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
  /* Pebbleboy controls which emulated frame is drawn. Peanut-GB's alternating
   * frame skip cannot distinguish a late compositor callback from a timely
   * one, so leaving it enabled would make the decoupled scheduler irregular. */
  s_gb->direct.frame_skip = false;
  s_gb->direct.interlace = false;

  size_t save_size = 0;
  if (gb_get_save_size_s(s_gb, &save_size) != 0 || save_size > PB_SAVE_MAX_SIZE) {
    prv_set_status("Unsupported save size");
    return false;
  }
  prv_set_status("Opening cartridge save");
  if (!pb_store_open(s_cart, prv_rom_checksum(), save_size)) {
    prv_set_status("Cannot open save storage");
    return false;
  }
  s_sync_active = false;
  s_save_importing = false;
  s_save_revision = 0;
  prv_init_rtc();
  if (save_size > 0) {
    if (!prv_init_local_save(save_size)) {
      return false;
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "using local persistent SRAM: %u",
              (unsigned)save_size);
    }
  }

  char title[17];
  gb_get_rom_name(s_gb, title);
  snprintf(s_status, sizeof(s_status), "%s %s", title[0] ? title : "DMG ROM", source_name);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "started %s, save=%u, cache=%ux%u, heap free=%u used=%u",
          s_status, (unsigned)s_cart_ram_size, (unsigned)PB_CART_CACHE_SLOTS,
          (unsigned)PB_CART_LINE_SIZE, (unsigned)heap_bytes_free(),
          (unsigned)heap_bytes_used());
  s_frames = 0;
  s_last_log_frame = 0;
  s_last_log_ms = prv_now_ms();
  memset(&s_profile, 0, sizeof(s_profile));
  s_running = true;
  // Give the loader a chance to restore its save before the first emulated frame.
  // Offline starts proceed immediately; an unavailable companion cannot block play.
  s_save_sync_deadline = s_cart->mode == PB_CART_MODE_BLOB &&
      connection_service_peek_pebble_app_connection() ? prv_now_ms() + 5000 : 0;
  return true;
}

bool pb_core_should_pause(struct gb_s *gb) {
  return gb == s_gb && s_cart && s_cart->failed;
}

#ifdef PEBBLEBOY_APP_BLOB
static void prv_install_fail(const char *status) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "ROM install failed at %lu/%lu: %s",
          (unsigned long)s_rom_install.offset, (unsigned long)s_rom_install.size,
          status ? status : "unknown error");
  if (s_rom_install.active) {
    app_blob_delete();
  }
  memset(&s_rom_install, 0, sizeof(s_rom_install));
  pb_cart_init_empty(s_cart);
  prv_set_status(status ? status : "ROM install failed");
}

static bool prv_start_matching_blob(uint32_t size, uint32_t crc32) {
  AppBlobInfo info;
  if (app_blob_get_info(&info) != S_SUCCESS || info.size != size || info.crc32 != crc32) {
    return false;
  }

  if (s_cart->mode == PB_CART_MODE_BLOB && s_cart->rom_size == size && s_running) {
    return true;
  }

  prv_flush_local_save("ROM-change");
  if (s_cart_ram_dirty) return true;
  prv_free_save_ram();
  if (!pb_cart_init_blob(s_cart, size)) {
    prv_set_status(s_cart->error);
    return true;
  }
  prv_start_from_cart("flash");
  return true;
}

static void prv_begin_install(const PbPhoneEvent *event) {
  s_running = false;
  pb_audio_suspend_stream();
  prv_flush_local_save("ROM-change");
  if (s_cart_ram_dirty || !prv_flush_rtc("ROM-change")) {
    prv_set_status("Save storage failed");
    return;
  }
  s_rtc_active = false;
  prv_free_save_ram();
  pb_cart_init_empty(s_cart);
  memset(&s_rom_install, 0, sizeof(s_rom_install));

  status_t result = app_blob_begin(event->size);
  if (result != S_SUCCESS) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "app_blob_begin size=%lu failed: %ld free=%lu",
            (unsigned long)event->size, (long)result,
            (unsigned long)app_blob_get_free_size());
    prv_set_status(result == E_OUT_OF_STORAGE ? "Not enough watch flash"
                                              : "Flash install failed");
    return;
  }

  s_rom_install.active = true;
  s_rom_install.size = event->size;
  s_rom_install.crc32 = event->crc32;
  snprintf(s_rom_install.title, sizeof(s_rom_install.title), "%s",
           event->title[0] ? event->title : "ROM");
  snprintf(s_status, sizeof(s_status), "Installing %s 0%%", s_rom_install.title);
  layer_mark_dirty(s_canvas);
  if (!gb_phone_install_ack(0)) {
    prv_install_fail("Phone link busy");
  }
}

static void prv_install_data(const PbPhoneEvent *event) {
  if (!s_rom_install.active) {
    return;
  }
  if (event->offset != s_rom_install.offset) {
    if (event->offset < s_rom_install.offset) {
      gb_phone_install_ack(s_rom_install.offset);
      return;
    }
    prv_install_fail("ROM chunks out of order");
    return;
  }
  if (!event->data_len || event->data_len > 512u ||
      event->offset + event->data_len > s_rom_install.size) {
    prv_install_fail("Invalid ROM chunk");
    return;
  }

  int written = app_blob_write(event->offset, event->data, event->data_len);
  if (written != event->data_len) {
    prv_install_fail("Watch flash write failed");
    return;
  }
  s_rom_install.offset += event->data_len;
  if ((s_rom_install.offset & 0xFFFFu) == 0 ||
      s_rom_install.offset == s_rom_install.size) {
    unsigned percent = (unsigned)((s_rom_install.offset * 100u) / s_rom_install.size);
    snprintf(s_status, sizeof(s_status), "Installing %s %u%%",
             s_rom_install.title, percent);
    layer_mark_dirty(s_canvas);
  }
  if (!gb_phone_install_ack(s_rom_install.offset)) {
    prv_install_fail("Phone link busy");
  }
}

static void prv_finish_install(const PbPhoneEvent *event) {
  if (!s_rom_install.active) {
    AppBlobInfo info;
    if (app_blob_get_info(&info) == S_SUCCESS && info.crc32 == event->crc32) {
      gb_phone_install_done();
    }
    return;
  }
  if (s_rom_install.offset != s_rom_install.size || event->crc32 != s_rom_install.crc32) {
    prv_install_fail("ROM transfer incomplete");
    return;
  }

  prv_set_status("Verifying ROM");
  status_t result = app_blob_commit(s_rom_install.crc32);
  if (result != S_SUCCESS) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "app_blob_commit failed: %ld", (long)result);
    prv_install_fail("ROM verification failed");
    return;
  }

  uint32_t size = s_rom_install.size;
  memset(&s_rom_install, 0, sizeof(s_rom_install));
  if (!pb_cart_init_blob(s_cart, size)) {
    prv_set_status(s_cart->error);
  } else {
    prv_start_from_cart("flash");
  }
  gb_phone_install_done();
}
#endif

static void prv_sync_error(uint32_t token, const char *message) {
  if (token == s_sync_token) {
    s_save_importing = false;
    s_sync_active = false;
  }
  gb_phone_save_send(PB_CMD_SAVE_ERROR, token, 0, 0, 0, NULL, message, NULL, 0, 0, false);
}

static bool prv_sync_read(uint32_t offset, uint8_t *data, size_t *size) {
  if (offset == s_cart_ram_size) {
    *size = sizeof(s_sync_rtc);
    memcpy(data, s_sync_rtc, *size);
    return true;
  }
  if (offset > s_cart_ram_size || offset % PB_SAVE_CHUNK_SIZE) return false;
  *size = s_cart_ram_size - offset;
  if (*size > PB_SAVE_CHUNK_SIZE) *size = PB_SAVE_CHUNK_SIZE;
  return pb_store_read_chunk(offset, data, *size) == (int)*size;
}

static void prv_sync_info(void) {
  char title[17];
  gb_get_rom_name(s_gb, title);
  gb_phone_save_send(PB_CMD_SAVE_INFO, s_sync_token, 0, s_cart_ram_size,
                     s_sync_crc, pb_store_id(), title, NULL, 0, pb_store_updated_at(), pb_store_has_save());
}

static void prv_save_event(const PbPhoneEvent *event) {
  uint32_t token = event->bank;
  uint32_t total = s_cart_ram_size + PB_STORE_RTC_SIZE;
  if (event->command == PB_CMD_SAVE_ABORT) {
    if (token == s_sync_token) { s_save_importing = false; s_sync_active = false; s_save_sync_deadline = 0; }
    return;
  }
  if (!s_running || !pb_store_id()[0]) {
    prv_sync_error(token, "Open a game with saves");
    return;
  }
  if (event->command == PB_CMD_SAVE_REQUEST) {
    if (s_save_importing) return;
    if (event->offset == 1 || s_save_sync_deadline) {
      s_save_sync_deadline = prv_now_ms() + 15000;
      s_draw_frame_pending = false;
      pb_audio_suspend_stream();
    }
    if (s_sync_active && token == s_sync_token && s_sync_revision == s_save_revision) {
      s_sync_activity = prv_now_ms();
      prv_sync_info();
      return;
    }
    prv_flush_local_save("phone-backup");
    bool rtc_saved = prv_flush_rtc("phone-backup");
    if (!rtc_saved || s_cart_ram_dirty || pb_store_read_rtc(s_sync_rtc) != sizeof(s_sync_rtc)) {
      prv_sync_error(token, "Save storage failed");
      return;
    }
    uint32_t crc = 0xffffffffu;
    uint8_t page[PB_SAVE_CHUNK_SIZE];
    for (uint32_t offset = 0; offset < total;) {
      size_t size;
      if (!prv_sync_read(offset, page, &size)) {
        prv_sync_error(token, "Save read failed");
        return;
      }
      crc = pb_store_crc32(crc, page, size);
      offset += size;
    }
    s_sync_crc = crc ^ 0xffffffffu;
    s_sync_revision = s_save_revision;
    s_sync_token = token;
    s_sync_active = true;
    s_sync_committed = false;
    s_sync_activity = prv_now_ms();
    prv_sync_info();
    return;
  }
  if (event->command == PB_CMD_SAVE_BEGIN) {
    if (s_save_importing && token == s_sync_token) {
      gb_phone_save_send(PB_CMD_SAVE_ACK, token, s_sync_offset, 0, 0, NULL, NULL, NULL, 0, 0, false);
      return;
    }
    if (s_save_importing || strcmp(event->rom_id, pb_store_id()) || event->size != total || event->updated_at > 9007199254740991ULL) {
      prv_sync_error(token, "Wrong game or save size");
      return;
    }
    if (event->updated_at < pb_store_updated_at()) {
      prv_sync_error(token, "Newer watch save; sync again");
      return;
    }
    if (s_save_revision != s_sync_revision) {
      prv_sync_error(token, "Save changed; back up again");
      return;
    }
    prv_flush_local_save("before-import");
    if (s_cart_ram_dirty) { prv_sync_error(token, "Save storage failed"); return; }
    s_save_importing = true;
    prv_set_status("Importing save: keep app open");
    s_sync_active = true;
    s_sync_committed = false;
    s_sync_import_time = event->updated_at;
    s_sync_token = token;
    s_sync_offset = 0;
    s_sync_crc = 0xffffffffu;
    s_sync_activity = prv_now_ms();
    s_draw_frame_pending = false;
    pb_audio_suspend_stream();
    gb_phone_save_send(PB_CMD_SAVE_ACK, token, 0, 0, 0, NULL, NULL, NULL, 0, 0, false);
    return;
  }
  if (token != s_sync_token) { prv_sync_error(token, "Transfer expired"); return; }
  if (event->command == PB_CMD_SAVE_COMMIT && s_sync_committed) {
    gb_phone_save_send(PB_CMD_SAVE_ACK, token, total, 0, event->crc32, NULL, "Imported", NULL, 0, 0, false);
    return;
  }
  if (!s_sync_active) { prv_sync_error(token, "Transfer expired"); return; }
  s_sync_activity = prv_now_ms();
  if (s_save_sync_deadline) s_save_sync_deadline = s_sync_activity + 15000;
  if (event->command == PB_CMD_SAVE_READ && !s_save_importing) {
    if (s_save_revision != s_sync_revision) {
      prv_sync_error(token, "Save changed; retry");
      return;
    }
    uint8_t page[PB_SAVE_CHUNK_SIZE];
    size_t size;
    if (!prv_sync_read(event->offset, page, &size)) {
      prv_sync_error(token, "Invalid save offset");
      return;
    }
    gb_phone_save_send(PB_CMD_SAVE_DATA, token, event->offset, s_cart_ram_size,
                       s_sync_crc, NULL, NULL, page, size, 0, false);
  } else if (event->command == PB_CMD_SAVE_WRITE && s_save_importing) {
    if (event->offset < s_sync_offset) {
      gb_phone_save_send(PB_CMD_SAVE_ACK, token, s_sync_offset, 0, 0, NULL, NULL, NULL, 0, 0, false);
      return;
    }
    uint32_t expected = event->offset == s_cart_ram_size ? PB_STORE_RTC_SIZE : PB_SAVE_CHUNK_SIZE;
    if (event->offset != s_sync_offset || event->offset >= total ||
        event->data_len != expected || event->offset + expected > total) {
      prv_sync_error(token, "Invalid save chunk");
      return;
    }
    bool stored;
    if (event->offset == s_cart_ram_size) {
      PbRtcPersist rtc;
      memcpy(&rtc, event->data, sizeof(rtc));
      if (s_rtc_active && (rtc.magic != PB_RTC_MAGIC || rtc.version != PB_RTC_VERSION ||
          rtc.rom_checksum != prv_rom_checksum() || rtc.saved_at_inverse != ~rtc.saved_at ||
          !prv_rtc_registers_valid(rtc.registers))) {
        prv_sync_error(token, "Invalid clock data");
        return;
      }
      stored = pb_store_stage_rtc(event->data);
    } else {
      stored = pb_store_stage(event->offset, event->data, event->data_len);
    }
    if (!stored) { prv_sync_error(token, "Watch storage full"); return; }
    s_sync_crc = pb_store_crc32(s_sync_crc, event->data, event->data_len);
    s_sync_offset += event->data_len;
    gb_phone_save_send(PB_CMD_SAVE_ACK, token, s_sync_offset, 0, 0, NULL, NULL, NULL, 0, 0, false);
  } else if (event->command == PB_CMD_SAVE_COMMIT && s_save_importing) {
    if (s_sync_offset != total || (s_sync_crc ^ 0xffffffffu) != event->crc32 ||
        !pb_store_commit(s_sync_import_time)) {
      prv_sync_error(token, "Save validation failed");
      return;
    }
    /* Do not flush the old emulated clock or RAM over the newly selected bank. */
    s_rtc_active = false;
    prv_start_from_cart("imported save");
    s_sync_committed = true;
    s_save_sync_deadline = 0;
    gb_phone_save_send(PB_CMD_SAVE_ACK, token, total, 0, event->crc32, NULL, "Imported", NULL, 0, 0, false);
  }
}

static PB_SIZE_OPT void prv_phone_event(const PbPhoneEvent *event, void *context) {
  (void)context;
  if (s_save_importing && event->type != PB_PHONE_EVENT_SAVE &&
      event->type != PB_PHONE_EVENT_SETTINGS) return;
#ifdef PEBBLEBOY_APP_BLOB
  // A late library response must not interrupt a game resumed offline.
  if ((event->type == PB_PHONE_EVENT_INFO && event->command == PB_CMD_ROM_LAUNCH) ||
      (event->type == PB_PHONE_EVENT_ROM_LIST_BEGIN && event->command == PB_CMD_ROM_LIST_LAUNCH)) {
    s_cached_resume = false;
    s_settings_open = false;
    s_library_deadline = 0;
  }
  if (s_cached_resume && (event->type == PB_PHONE_EVENT_INFO ||
      event->type == PB_PHONE_EVENT_ROM_LIST_BEGIN ||
      event->type == PB_PHONE_EVENT_ROM_LIST_ITEM ||
      event->type == PB_PHONE_EVENT_ROM_LIST_END)) return;
#endif
  switch (event->type) {
    case PB_PHONE_EVENT_SETTINGS:
      s_settings_open = event->command == PB_CMD_SETTINGS_OPEN;
      if (s_settings_open) s_auto_launch_deadline = 0;
      s_draw_frame_pending = false;
      pb_audio_suspend_stream();
      pb_input_release(0xff);
      if (s_canvas) {
        if (s_settings_open) layer_set_frame(s_canvas, layer_get_bounds(window_get_root_layer(s_window)));
        else prv_set_rom_selector(s_rom_selector);
        layer_mark_dirty(s_canvas);
      }
      APP_LOG(APP_LOG_LEVEL_INFO, "phone settings %s", s_settings_open ? "open" : "closed");
      break;
    case PB_PHONE_EVENT_SAVE:
      prv_save_event(event);
      break;
    case PB_PHONE_EVENT_ROM_LIST_BEGIN:
      s_waiting_launch = true;
      s_auto_launch_deadline = 0;
      s_rom_selection_pending = false;
      s_draw_frame_pending = false;
      s_phone_offer_seen = false;
      pb_audio_suspend_stream();
      pb_input_release(0xff);
      prv_set_rom_selector(false);
      memset(s_rom_titles, 0, sizeof(s_rom_titles));
      s_rom_count = (uint8_t)(event->size > ROM_LIBRARY_MAX
                                  ? ROM_LIBRARY_MAX
                                  : event->size);
      s_rom_selected = 0;
      s_phone_library_seen = false;
      prv_set_status("Loading ROM library");
      break;
    case PB_PHONE_EVENT_ROM_LIST_ITEM:
      if (event->bank < ROM_LIBRARY_MAX) {
        snprintf(s_rom_titles[event->bank], sizeof(s_rom_titles[event->bank]),
                 "%s", event->title[0] ? event->title : "DMG ROM");
      }
      break;
    case PB_PHONE_EVENT_ROM_LIST_END:
#ifdef PEBBLEBOY_APP_BLOB
      s_library_deadline = 0;
#endif
      s_rom_count = (uint8_t)(event->size > ROM_LIBRARY_MAX
                                  ? ROM_LIBRARY_MAX
                                  : event->size);
      s_phone_library_seen = true;
      if (s_rom_count == 0) {
        s_rom_selection_pending = false;
        prv_set_status("Add ROMs in app settings");
      } else if (s_rom_count == 1 && !s_settings_open) {
        // Give showConfiguration time to suppress a normal one-game launch.
        s_auto_launch_deadline = prv_now_ms() + 1000;
        prv_set_status("Loading ROM");
      } else {
        s_rom_selection_pending = false;
        prv_set_rom_selector(true);
      }
      if (s_canvas) {
        layer_mark_dirty(s_canvas);
      }
      break;
    case PB_PHONE_EVENT_INFO:
#ifdef PEBBLEBOY_APP_BLOB
      if (s_waiting_launch && event->command != PB_CMD_ROM_LAUNCH &&
          !s_rom_selection_pending) break;
      s_waiting_launch = false;
      s_phone_offer_seen = true;
      s_rom_selection_pending = false;
      prv_set_rom_selector(false);
      APP_LOG(APP_LOG_LEVEL_INFO,
              "phone info title=%s size=%lu cart=%u crc=%08lx scale=%u",
              event->title[0] ? event->title : "ROM", (unsigned long)event->size,
              (unsigned)event->cart_type, (unsigned long)event->crc32,
              (unsigned)event->video_scale);
      if (s_rom_install.active) {
        if (s_rom_install.size == event->size &&
            s_rom_install.crc32 == event->crc32) {
          APP_LOG(APP_LOG_LEVEL_INFO,
                  "ignoring duplicate ROM info during install at %lu/%lu",
                  (unsigned long)s_rom_install.offset,
                  (unsigned long)s_rom_install.size);
        } else {
          prv_install_fail("ROM changed during install");
        }
        break;
      }
      prv_apply_phone_settings(event);
      if (prv_start_matching_blob(event->size, event->crc32)) {
        APP_LOG(APP_LOG_LEVEL_INFO, "configured ROM already installed");
        gb_phone_install_done();
      } else {
        prv_begin_install(event);
      }
      break;
#else
      s_phone_offer_seen = true;
      s_rom_selection_pending = false;
      prv_set_rom_selector(false);
      prv_apply_phone_settings(event);
      if (!s_running) {
        prv_set_status("URL installs require CFW");
      }
      break;
#endif
    case PB_PHONE_EVENT_INSTALL_DATA:
#ifdef PEBBLEBOY_APP_BLOB
      prv_install_data(event);
#endif
      break;
    case PB_PHONE_EVENT_INSTALL_END:
#ifdef PEBBLEBOY_APP_BLOB
      prv_finish_install(event);
#endif
      break;
    case PB_PHONE_EVENT_ERROR:
      s_phone_offer_seen = true;
      s_rom_selection_pending = false;
#ifdef PEBBLEBOY_APP_BLOB
      if (s_rom_install.active) {
        prv_install_fail(event->status[0] ? event->status : "Phone install error");
        break;
      }
#endif
      prv_set_status(event->status[0] ? event->status : "No ROM URL");
      break;
  }
}

static void prv_log_perf(void) {
  uint64_t now = prv_now_ms();
  if (now - s_last_log_ms < 2000) {
    return;
  }
  const PbCartStats *stats = pb_cart_stats(s_cart);
  const PbAudioStats *audio_stats = pb_audio_stats();
  uint32_t frame_delta = s_frames - s_last_log_frame;
  uint32_t ms_delta = (uint32_t)(now - s_last_log_ms);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "fps=%u cache m=%lu fills=%lu last=%u/%u heap=%u/%u",
          (unsigned)((frame_delta * 1000u) / (ms_delta ? ms_delta : 1)),
          stats->misses, stats->loads,
          (unsigned)stats->last_miss_bank, (unsigned)stats->last_load_bank,
          (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());
  APP_LOG(APP_LOG_LEVEL_INFO,
          "io reads=%lu bytes=%lu save dirty=%u reads=%u/%lu writes=%u/%lu audio=%lu/%lu/%lu",
          stats->source_reads, stats->source_bytes,
          (unsigned)(s_cart_ram ? pb_save_dirty_count(&s_local_save) : 0),
          (unsigned)(s_cart_ram ? s_local_save.read_ops : 0),
          (unsigned long)(s_cart_ram ? s_local_save.read_bytes : 0),
          (unsigned)(s_cart_ram ? s_local_save.write_ops : 0),
          (unsigned long)(s_cart_ram ? s_local_save.write_bytes : 0),
          audio_stats->pumps, audio_stats->partial_writes,
          audio_stats->stream_errors);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "mix gen=%lu nz=%u/%u peak=%u ch=%x nr50=%02x nr51=%02x nr52=%02x",
          audio_stats->generated_buffers,
          (unsigned)audio_stats->last_nonzero_samples,
          (unsigned)PB_AUDIO_PUMP_SAMPLES, (unsigned)audio_stats->last_peak,
          (unsigned)audio_stats->active_channels, (unsigned)audio_stats->nr50,
          (unsigned)audio_stats->nr51, (unsigned)audio_stats->nr52);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "profile ms cpu=%lu audio=%lu save=%lu render=%lu tick=%lu/%lu max=%lu present=%lu draw=%lu same=%lu catch=%lu",
          (unsigned long)s_profile.cpu_ms, (unsigned long)s_profile.audio_ms,
          (unsigned long)s_profile.save_ms, (unsigned long)s_profile.render_ms,
          (unsigned long)s_profile.tick_ms, (unsigned long)s_profile.ticks,
          (unsigned long)s_profile.max_tick_ms, (unsigned long)s_profile.presented,
          (unsigned long)s_profile.renders, (unsigned long)s_profile.unchanged,
          (unsigned long)s_profile.catchups);
  s_last_log_ms = now;
  s_last_log_frame = s_frames;
  memset(&s_profile, 0, sizeof(s_profile));
}

static void prv_maybe_request_phone_info(uint64_t now) {
  if (s_phone_offer_seen) {
    return;
  }
  if (now - s_last_phone_info_request_ms < PHONE_INFO_RETRY_MS) {
    return;
  }
  if (s_waiting_launch && !s_rom_selection_pending) {
    if (!s_phone_library_seen) gb_phone_request_rom_list();
  } else if (s_cart && s_cart->mode == PB_CART_MODE_BLOB) {
    gb_phone_request_info();
  } else if (s_cart && s_cart->mode != PB_CART_MODE_NONE) {
    return;
  } else if (!s_phone_library_seen) {
    gb_phone_request_rom_list();
  } else if (s_rom_selection_pending) {
    gb_phone_select_rom(s_rom_selected);
  } else {
    return;
  }
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
  s_gb->gb_frame = false;

  while (!s_gb->gb_frame) {
    __gb_step_cpu(s_gb);
  }
  if (s_cart->failed) {
    return false;
  }
  s_frames++;
  return true;
}

static bool prv_run_profiled_frame(bool draw) {
  s_gb->display.lcd_draw_line = draw ? prv_lcd_draw_line : NULL;
  uint64_t phase_start_ms = prv_now_ms();
  bool completed = prv_run_one_frame();
  s_profile.cpu_ms += prv_elapsed_ms(phase_start_ms);
  return completed;
}

static void prv_record_tick_time(uint64_t started_ms) {
  uint32_t elapsed_ms = prv_elapsed_ms(started_ms);
  s_profile.tick_ms += elapsed_ms;
  if (elapsed_ms > s_profile.max_tick_ms) {
    s_profile.max_tick_ms = elapsed_ms;
  }
}

static void prv_frame_timer_cb(void *data) {
  (void)data;
  s_timer = NULL;
  uint64_t tick_start_ms = prv_now_ms();
#ifdef PEBBLEBOY_APP_BLOB
  if (s_library_deadline && tick_start_ms >= s_library_deadline &&
      !s_phone_library_seen && !s_running && !s_rom_install.active) {
    s_library_deadline = 0;
    s_cached_resume = true;
    s_phone_offer_seen = true;
    prv_set_rom_selector(false);
    if (pb_cart_init_blob(s_cart, s_cached_rom_size)) prv_start_from_cart("offline");
    s_save_sync_deadline = 0;
  }
#endif
  if (s_auto_launch_deadline && tick_start_ms >= s_auto_launch_deadline) {
    s_auto_launch_deadline = 0;
    if (!s_settings_open && s_rom_count == 1) {
      s_rom_selected = 0;
      s_rom_selection_pending = true;
      prv_set_rom_selector(false);
      gb_phone_select_rom(0);
      s_last_phone_info_request_ms = tick_start_ms;
    }
  }
  prv_maybe_request_phone_info(tick_start_ms);

  if (s_sync_active && tick_start_ms - s_sync_activity > 15000) {
    s_sync_active = false;
    s_save_importing = false;
    s_save_sync_deadline = 0;
  }
  if (s_running && !s_save_importing && tick_start_ms - s_last_save_flush >= 2000) {
    prv_flush_local_save("cartridge");
    s_last_save_flush = tick_start_ms;
  }
  bool can_run = s_running && !s_waiting_launch && !s_settings_open && s_in_focus && !s_cart->failed && !s_save_importing &&
      tick_start_ms >= s_save_sync_deadline;
  if (can_run) {
    if (s_fast_forward) {
      pb_audio_suspend_stream();
    }

    uint8_t frames_per_tick = s_fast_forward
                                  ? FAST_FRAMES_PER_TICK
                                  : NORMAL_FRAMES_PER_TICK;
    uint8_t draw_credit = s_draw_frame_credit < frames_per_tick
                              ? s_draw_frame_credit
                              : frames_per_tick;
    s_draw_frame_credit = 0;
    if (s_draw_frame_pending) {
      s_profile.unchanged++;
    }
    s_draw_frame_pending = false;

    /* A drawn frame is real emulation work, so compensate for it at the next
     * 60 Hz timer. If presentation was coalesced, run that frame headlessly.
     * Audio is owned exclusively by this evenly paced timer; compositor timing
     * must never bunch two writes together or leave a gap in the PCM stream. */
    uint8_t headless_target = (uint8_t)(frames_per_tick - draw_credit);
    uint8_t completed = 0;
    while (completed < headless_target) {
      if (!prv_run_profiled_frame(false)) {
        break;
      }
      completed++;
      if (s_fast_forward && completed >= NORMAL_FRAMES_PER_TICK &&
          prv_elapsed_ms(tick_start_ms) >= FRAME_BASE_MS) {
        break;
      }
    }

    if (!s_fast_forward) {
      uint64_t phase_start_ms = prv_now_ms();
      if (draw_credit || completed) {
        pb_audio_pump();
      } else {
        pb_audio_pump_silence();
      }
      s_profile.audio_ms += prv_elapsed_ms(phase_start_ms);
    }

    if (!s_cart->failed && s_canvas) {
      s_draw_frame_pending = true;
      layer_mark_dirty(s_canvas);
    }
  } else {
    s_draw_frame_pending = false;
    s_draw_frame_credit = 0;
    if (s_running && !s_waiting_launch && !s_settings_open && s_in_focus) {
      pb_audio_pump_silence();
    } else {
      pb_audio_suspend_stream();
    }
  }

  uint64_t tick_end_ms = prv_now_ms();
  if (s_running) {
    prv_record_tick_time(tick_start_ms);
    s_profile.ticks++;
    prv_log_perf();
  }
  if (!s_next_frame_deadline_ms) {
    s_next_frame_deadline_ms = tick_start_ms;
    s_frame_deadline_remainder = 0;
  }
  prv_advance_frame_deadline();
  if (tick_end_ms > s_next_frame_deadline_ms + FRAME_CATCHUP_LIMIT_MS) {
    prv_reset_frame_deadline(tick_end_ms, false);
  }
  uint32_t delay = 1;
  if (tick_end_ms < s_next_frame_deadline_ms) {
    delay = (uint32_t)(s_next_frame_deadline_ms - tick_end_ms);
  } else {
    s_profile.catchups++;
  }
  prv_schedule_frame_timer(delay);
}

static PB_SIZE_OPT void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  uint64_t render_start_ms = prv_now_ms();
  GRect bounds = layer_get_bounds(layer);
  GRect root_frame = layer_get_frame(window_get_root_layer(s_window));
  PbVideoScale scale = pb_video_scale();
  GRect render_bounds = scale == PB_VIDEO_SCALE_1X
                            ? layer_get_frame(layer)
                            : root_frame;
  if (scale == PB_VIDEO_SCALE_1X) {
    render_bounds.origin.x += root_frame.origin.x;
    render_bounds.origin.y += root_frame.origin.y;
  }
  if (s_draw_frame_pending && s_running && !s_waiting_launch && !s_settings_open && s_in_focus && !s_cart->failed && !s_save_importing && prv_now_ms() >= s_save_sync_deadline) {
    uint64_t tick_start_ms = prv_now_ms();
    if (pb_video_begin_frame(ctx, render_bounds)) {
      bool completed = prv_run_profiled_frame(true);
      pb_video_end_frame(ctx);
      s_draw_frame_pending = false;
      if (completed) {
        s_profile.presented++;
        if (s_draw_frame_credit < UINT8_MAX) {
          s_draw_frame_credit++;
        }
      }
      prv_record_tick_time(tick_start_ms);
    }
    render_start_ms = prv_now_ms();
  }

  if (s_settings_open) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "Settings open\non phone",
                       fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(8, bounds.size.h / 2 - 48, bounds.size.w - 16, 64),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "Tap Save and launch\nwhen ready",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(8, bounds.size.h / 2 + 24, bounds.size.w - 16, 56),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_rom_selector) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "Choose a game",
                       fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(4, 4, bounds.size.w - 8, 32),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    uint8_t first = 0;
    if (s_rom_selected >= ROM_SELECTOR_VISIBLE_ROWS) {
      first = (uint8_t)(s_rom_selected - ROM_SELECTOR_VISIBLE_ROWS + 1);
    }
    if (first + ROM_SELECTOR_VISIBLE_ROWS > s_rom_count &&
        s_rom_count > ROM_SELECTOR_VISIBLE_ROWS) {
      first = (uint8_t)(s_rom_count - ROM_SELECTOR_VISIBLE_ROWS);
    }
    for (uint8_t row = 0; row < ROM_SELECTOR_VISIBLE_ROWS; row++) {
      uint8_t index = (uint8_t)(first + row);
      if (index >= s_rom_count) {
        break;
      }
      GRect item = GRect(8, 40 + row * 30, bounds.size.w - 16, 28);
      bool selected = index == s_rom_selected;
      if (selected) {
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, item, 4, GCornersAll);
      }
      graphics_context_set_text_color(ctx, selected ? GColorBlack : GColorWhite);
      graphics_draw_text(ctx,
                         s_rom_titles[index][0] ? s_rom_titles[index] : "DMG ROM",
                         fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), item,
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "UP/DOWN choose  SELECT play",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(4, bounds.size.h - 28, bounds.size.w - 8, 24),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    s_profile.render_ms += prv_elapsed_ms(render_start_ms);
    s_profile.renders++;
    return;
  }

  if (s_running && s_fast_forward) {
    GRect badge = GRect(bounds.size.w - 30, 2, 28, 20);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, badge, 3, GCornersAll);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "2x", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       badge, GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }

  if (s_save_importing || s_waiting_launch || !s_running || !s_cart || s_cart->failed) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    int16_t status_y = bounds.size.h - 34;
    int16_t status_h = 30;
    if (scale == PB_VIDEO_SCALE_ASPECT_FIT) {
      int16_t video_h = (int16_t)((int32_t)bounds.size.w * PB_GB_LCD_H /
                                  PB_GB_LCD_W);
      status_y = (bounds.size.h + video_h) / 2;
      status_h = bounds.size.h - status_y;
    }
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_status, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(4, status_y, bounds.size.w - 8, status_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
  s_profile.render_ms += prv_elapsed_ms(render_start_ms);
  s_profile.renders++;
}

static void prv_select_rom_delta(int delta) {
  if (!s_rom_selector || s_rom_count == 0) {
    return;
  }
  int selected = (int)s_rom_selected + delta;
  if (selected < 0) {
    selected = s_rom_count - 1;
  } else if (selected >= s_rom_count) {
    selected = 0;
  }
  s_rom_selected = (uint8_t)selected;
  layer_mark_dirty(s_canvas);
}

static void prv_launch_selected_rom(void) {
  if (s_settings_open || !s_rom_selector || s_rom_selected >= s_rom_count) {
    return;
  }
  s_rom_selection_pending = true;
  prv_set_rom_selector(false);
  prv_set_status("Loading selected ROM");
  gb_phone_select_rom(s_rom_selected);
  s_last_phone_info_request_ms = prv_now_ms();
}

static PB_SIZE_OPT void prv_raw_down_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  uint8_t mask = (uint8_t)(uintptr_t)context;
  if (s_settings_open) return;
  if (s_rom_selector) {
    if (mask == JOYPAD_START) {
      prv_select_rom_delta(-1);
    } else if (mask == JOYPAD_B) {
      prv_select_rom_delta(1);
    } else if (mask == JOYPAD_A) {
      prv_launch_selected_rom();
    }
    return;
  }
  pb_input_release_touch();
  pb_input_press(mask);
}

static void prv_raw_up_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  if (s_rom_selector) {
    return;
  }
  pb_input_release((uint8_t)(uintptr_t)context);
}

static void prv_back_release_cb(void *data) {
  (void)data;
  pb_input_release(JOYPAD_SELECT);
}

static void prv_back_click_handler(ClickRecognizerRef ref, void *context) {
  (void)ref;
  (void)context;
  if (s_rom_selector) {
    window_stack_pop(true);
    return;
  }
  pb_input_release_touch();
  if (pb_input_start_select_toggle((uint32_t)prv_now_ms())) {
    prv_set_fast_forward(!s_fast_forward);
  }
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
  if (s_rom_selector) {
    prv_touch_release();
    return;
  }
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

static PB_SIZE_OPT void prv_window_load(Window *window) {
  WatchInfoVersion fw = watch_info_get_firmware_version();
  APP_LOG(APP_LOG_LEVEL_INFO, "watch firmware=%u.%u.%u model=%u",
          (unsigned)fw.major, (unsigned)fw.minor, (unsigned)fw.patch,
          (unsigned)watch_info_get_model());

  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, GColorBlack);
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas);

  if (!pb_video_init()) {
    prv_set_status("No video heap");
    return;
  }
  prv_update_canvas_frame();
  pb_input_init();
  pb_audio_init();
  if (persist_exists(PB_PREF_SCALE)) {
    int scale = persist_read_int(PB_PREF_SCALE);
    if (scale >= PB_VIDEO_SCALE_1X && scale <= PB_VIDEO_SCALE_ASPECT_FIT) {
      pb_video_set_scale((PbVideoScale)scale);
      prv_update_canvas_frame();
    }
  }
  if (persist_exists(PB_PREF_AUDIO)) {
    pb_audio_set_enabled(persist_read_int(PB_PREF_AUDIO) != 0);
  }
  s_in_focus = true;
  s_waiting_launch = false;
  s_settings_open = false;
  s_auto_launch_deadline = 0;
  app_focus_service_subscribe(prv_focus_handler);
  s_cart = malloc(sizeof(*s_cart));
  s_gb = malloc(sizeof(*s_gb));
  if (!s_cart || !s_gb) {
    prv_set_status("Out of memory");
    return;
  }
  pb_cart_init_empty(s_cart);
  s_phone_offer_seen = false;
  s_phone_library_seen = false;
  s_rom_selector = false;
  s_rom_selection_pending = false;
  s_rom_count = 0;
  s_rom_selected = 0;
  memset(s_rom_titles, 0, sizeof(s_rom_titles));
  s_last_phone_info_request_ms = 0;
#ifdef PEBBLEBOY_APP_BLOB
  memset(&s_rom_install, 0, sizeof(s_rom_install));
  s_cached_rom_size = 0;
  s_library_deadline = 0;
  s_cached_resume = false;
#endif
  s_rtc_active = false;

  gb_phone_init(s_cart, prv_phone_event, NULL);

  bool started_local = false;
#ifdef RESOURCE_ID_CARTRIDGE
  prv_set_status("Loading local ROM");
  if (pb_cart_init_resource(s_cart, RESOURCE_ID_CARTRIDGE)) {
    started_local = prv_start_from_cart("flash");
    if (started_local) {
      pb_audio_set_enabled(true);
    }
  }
  if (!started_local) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "local ROM failed, falling back to phone");
    pb_cart_init_empty(s_cart);
  }
#endif

#ifdef PEBBLEBOY_APP_BLOB
  if (!started_local) {
    AppBlobInfo info;
    if (app_blob_get_info(&info) == S_SUCCESS) {
      s_cached_rom_size = info.size;
      if (!connection_service_peek_pebble_app_connection()) {
        if (pb_cart_init_blob(s_cart, info.size)) started_local = prv_start_from_cart("offline");
        if (started_local) {
          s_cached_resume = true;
          s_phone_offer_seen = true;
        } else pb_cart_init_empty(s_cart);
      } else {
        // Prepare the stored cartridge for save sync without running emulation.
        s_waiting_launch = true;
        if (pb_cart_init_blob(s_cart, info.size)) prv_start_from_cart("cached");
        pb_audio_suspend_stream();
      }
    }
  }
#endif

  if (!started_local) {
    s_waiting_launch = true;
    prv_set_status("Loading ROM library");
    gb_phone_request_rom_list();
    s_last_phone_info_request_ms = prv_now_ms();
  }

#ifdef PBL_TOUCH
  touch_service_subscribe(prv_touch_handler, NULL);
#endif

  uint64_t now_ms = prv_now_ms();
  prv_reset_frame_deadline(now_ms, false);
  prv_schedule_frame_timer((uint32_t)(s_next_frame_deadline_ms - now_ms));
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
  app_focus_service_unsubscribe();
  gb_phone_deinit();
  pb_audio_deinit();
  pb_video_deinit();
  prv_flush_local_save("shutdown");
  prv_flush_rtc("shutdown");
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
