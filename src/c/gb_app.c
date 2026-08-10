#include "gb_app.h"

#include <pebble.h>
#include <stdio.h>
#include <string.h>

#include "gb_audio.h"
#include "gb_cart.h"
#include "gb_core.h"
#include "gb_input.h"
#include "gb_phone.h"
#include "gb_save.h"
#include "gb_video.h"

#define FRAME_MS 33
#define FRAME_CATCHUP_LIMIT_MS (FRAME_MS * 4)
#define FRAMES_PER_TICK 2
#define MAX_CPU_STEPS_PER_TICK 40000u
#define PHONE_INFO_RETRY_MS 2000
#define CART_RAM_WINDOW_SIZE 0x1000u
#define CART_RAM_BANK_NONE UINT16_MAX
#define CART_RAM_SAVE_SETTLE_MS 30000
#define ROM_LIBRARY_MAX 12u
#define ROM_SELECTOR_VISIBLE_ROWS 5u

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
static uint16_t s_cart_ram_bank;
static bool s_cart_ram_dirty;
static bool s_cart_ram_paused;
static bool s_cart_ram_faulted;
static bool s_cart_ram_loading;
static bool s_cart_ram_saving;
static uint16_t s_cart_ram_loading_bank;
static uint16_t s_cart_ram_pending_bank;
static uint16_t s_cart_ram_loaded_bytes;
static uint64_t s_cart_ram_dirty_ms;
static bool s_cart_ram_local;
static PbSave s_local_save;
static char s_status[80];
static uint32_t s_frames;
static uint64_t s_last_log_ms;
static uint32_t s_last_log_frame;
static uint64_t s_last_phone_info_request_ms;
static uint32_t s_frame_budget_hits;
static uint64_t s_next_frame_deadline_ms;
static bool s_phone_bank_load_pending;
static uint16_t s_phone_bank_load_bank;
static uint16_t s_phone_bank_load_offset;
static uint16_t s_phone_bank_load_size;
static bool s_phone_bank_load_demand;
static uint64_t s_phone_bank_load_ms;

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
  bool gb_halt;
  bool gb_ime;
  bool gb_frame;
  bool lcd_blank;
  bool cart_is_mbc3O;
  int8_t mbc;
  uint8_t cart_ram;
  uint16_t num_rom_banks_mask;
  uint8_t num_ram_banks;
  uint16_t selected_rom_bank;
  uint8_t cart_ram_bank;
  uint8_t enable_cart_ram;
  uint8_t cart_mode_select;
  struct cpu_registers_s cpu_reg;
  struct count_s counter;
  uint8_t hram_io[HRAM_IO_SIZE];
  uint8_t oam[OAM_SIZE];
  uint8_t bg_palette[4];
  uint8_t sp_palette[8];
  uint8_t window_clear;
  uint8_t WY;
  bool frame_skip_count;
  bool interlace_count;
} PbGbFrameCheckpoint;

static PbGbFrameCheckpoint s_frame_checkpoint;

static void prv_frame_timer_cb(void *data);
static void prv_schedule_frame_timer(uint32_t delay_ms);
static void prv_update_canvas_frame(void);

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

static uint8_t prv_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  PbCart *cart = (PbCart *)gb->direct.priv;
  return pb_cart_read(cart, (uint32_t)addr);
}

static void prv_cart_ram_show_status(const char *action, uint16_t bank) {
  snprintf(s_status, sizeof(s_status), "%s SRAM %u", action, (unsigned)bank);
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static bool prv_ensure_cart_ram_window(void) {
  if (s_cart_ram) {
    return true;
  }
  if (!s_cart_ram_size || !s_cart_ram_window_size) {
    return false;
  }

  s_cart_ram = malloc(s_cart_ram_window_size);
  if (!s_cart_ram) {
    s_cart_ram_window_size = 0;
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "phone SRAM window allocation failed for %u bytes",
            (unsigned)s_cart_ram_size);
    prv_set_status("No SRAM heap");
    return false;
  }

  memset(s_cart_ram, 0xFF, s_cart_ram_window_size);
  s_cart_ram_bank = CART_RAM_BANK_NONE;
  s_cart_ram_loading_bank = CART_RAM_BANK_NONE;
  s_cart_ram_pending_bank = CART_RAM_BANK_NONE;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "phone SRAM window allocated: %u/%u bytes, heap free=%u used=%u",
          (unsigned)s_cart_ram_window_size, (unsigned)s_cart_ram_size,
          (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());
  return true;
}

static bool prv_start_cart_ram_load(uint16_t bank) {
  if (!prv_ensure_cart_ram_window() || s_cart_ram_loading || s_cart_ram_saving) {
    return false;
  }
  if (!gb_phone_request_sram_load(bank, (uint16_t)s_cart_ram_window_size,
                                  (uint32_t)s_cart_ram_size)) {
    return false;
  }

  memset(s_cart_ram, 0xFF, s_cart_ram_window_size);
  s_cart_ram_bank = CART_RAM_BANK_NONE;
  s_cart_ram_loading = true;
  s_cart_ram_loading_bank = bank;
  s_cart_ram_loaded_bytes = 0;
  s_cart_ram_paused = true;
  prv_cart_ram_show_status("Loading", bank);
  APP_LOG(APP_LOG_LEVEL_INFO, "phone SRAM load requested bank %u size=%u",
          (unsigned)bank, (unsigned)s_cart_ram_window_size);
  return true;
}

static bool prv_start_cart_ram_save(uint16_t pending_bank) {
  if (!s_cart_ram || s_cart_ram_bank == CART_RAM_BANK_NONE ||
      s_cart_ram_loading || s_cart_ram_saving) {
    return false;
  }
  if (!gb_phone_save_sram_bank(s_cart_ram_bank, s_cart_ram,
                               (uint16_t)s_cart_ram_window_size,
                               (uint32_t)s_cart_ram_size)) {
    return false;
  }

  s_cart_ram_saving = true;
  s_cart_ram_pending_bank = pending_bank;
  s_cart_ram_paused = true;
  prv_cart_ram_show_status("Saving", s_cart_ram_bank);
  APP_LOG(APP_LOG_LEVEL_INFO, "phone SRAM save started bank %u size=%u",
          (unsigned)s_cart_ram_bank, (unsigned)s_cart_ram_window_size);
  return true;
}

static bool prv_cart_ram_select_bank(uint16_t bank) {
  if (!s_cart_ram_size || !s_cart_ram_window_size) {
    return false;
  }
  if (!prv_ensure_cart_ram_window()) {
    return false;
  }
  if (bank == s_cart_ram_bank && !s_cart_ram_loading && !s_cart_ram_saving) {
    return true;
  }

  s_cart_ram_paused = true;
  s_cart_ram_faulted = true;
  s_cart_ram_pending_bank = bank;

  if (s_cart_ram_saving || s_cart_ram_loading) {
    return false;
  }
  if (s_cart_ram_dirty && s_cart_ram_bank != CART_RAM_BANK_NONE) {
    if (!prv_start_cart_ram_save(bank)) {
      prv_cart_ram_show_status("Saving", s_cart_ram_bank);
    }
    return false;
  }
  if (!prv_start_cart_ram_load(bank)) {
    prv_cart_ram_show_status("Loading", bank);
  }
  return false;
}

static uint8_t prv_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  if (addr < s_cart_ram_size) {
    if (s_cart_ram_local) {
      return pb_save_read(&s_local_save, addr);
    }
    uint16_t bank = (uint16_t)(addr / CART_RAM_WINDOW_SIZE);
    if (!prv_cart_ram_select_bank(bank)) {
      return 0xFF;
    }
    size_t offset = (size_t)(addr % CART_RAM_WINDOW_SIZE);
    if (offset < s_cart_ram_window_size) {
      return s_cart_ram[offset];
    }
  }
  return 0xFF;
}

static void prv_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t value) {
  (void)gb;
  if (addr < s_cart_ram_size) {
    if (s_cart_ram_local) {
      if (pb_save_write(&s_local_save, addr, value)) {
        s_cart_ram_dirty = true;
        s_cart_ram_dirty_ms = prv_now_ms();
      }
      return;
    }
    uint16_t bank = (uint16_t)(addr / CART_RAM_WINDOW_SIZE);
    if (!prv_cart_ram_select_bank(bank)) {
      return;
    }
    size_t offset = (size_t)(addr % CART_RAM_WINDOW_SIZE);
    if (offset < s_cart_ram_window_size) {
      s_cart_ram[offset] = value;
      s_cart_ram_dirty = true;
      s_cart_ram_dirty_ms = prv_now_ms();
    }
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

static void prv_save_frame_checkpoint(void) {
  s_frame_checkpoint.gb_halt = s_gb->gb_halt;
  s_frame_checkpoint.gb_ime = s_gb->gb_ime;
  s_frame_checkpoint.gb_frame = s_gb->gb_frame;
  s_frame_checkpoint.lcd_blank = s_gb->lcd_blank;
  s_frame_checkpoint.cart_is_mbc3O = s_gb->cart_is_mbc3O;
  s_frame_checkpoint.mbc = s_gb->mbc;
  s_frame_checkpoint.cart_ram = s_gb->cart_ram;
  s_frame_checkpoint.num_rom_banks_mask = s_gb->num_rom_banks_mask;
  s_frame_checkpoint.num_ram_banks = s_gb->num_ram_banks;
  s_frame_checkpoint.selected_rom_bank = s_gb->selected_rom_bank;
  s_frame_checkpoint.cart_ram_bank = s_gb->cart_ram_bank;
  s_frame_checkpoint.enable_cart_ram = s_gb->enable_cart_ram;
  s_frame_checkpoint.cart_mode_select = s_gb->cart_mode_select;
  s_frame_checkpoint.cpu_reg = s_gb->cpu_reg;
  s_frame_checkpoint.counter = s_gb->counter;
  memcpy(s_frame_checkpoint.hram_io, s_gb->hram_io, sizeof(s_frame_checkpoint.hram_io));
  memcpy(s_frame_checkpoint.oam, s_gb->oam, sizeof(s_frame_checkpoint.oam));
  memcpy(s_frame_checkpoint.bg_palette, s_gb->display.bg_palette,
         sizeof(s_frame_checkpoint.bg_palette));
  memcpy(s_frame_checkpoint.sp_palette, s_gb->display.sp_palette,
         sizeof(s_frame_checkpoint.sp_palette));
  s_frame_checkpoint.window_clear = s_gb->display.window_clear;
  s_frame_checkpoint.WY = s_gb->display.WY;
  s_frame_checkpoint.frame_skip_count = s_gb->display.frame_skip_count;
  s_frame_checkpoint.interlace_count = s_gb->display.interlace_count;
}

static void prv_restore_frame_checkpoint(void) {
  s_gb->gb_halt = s_frame_checkpoint.gb_halt;
  s_gb->gb_ime = s_frame_checkpoint.gb_ime;
  s_gb->gb_frame = s_frame_checkpoint.gb_frame;
  s_gb->lcd_blank = s_frame_checkpoint.lcd_blank;
  s_gb->cart_is_mbc3O = s_frame_checkpoint.cart_is_mbc3O;
  s_gb->mbc = s_frame_checkpoint.mbc;
  s_gb->cart_ram = s_frame_checkpoint.cart_ram;
  s_gb->num_rom_banks_mask = s_frame_checkpoint.num_rom_banks_mask;
  s_gb->num_ram_banks = s_frame_checkpoint.num_ram_banks;
  s_gb->selected_rom_bank = s_frame_checkpoint.selected_rom_bank;
  s_gb->cart_ram_bank = s_frame_checkpoint.cart_ram_bank;
  s_gb->enable_cart_ram = s_frame_checkpoint.enable_cart_ram;
  s_gb->cart_mode_select = s_frame_checkpoint.cart_mode_select;
  s_gb->cpu_reg = s_frame_checkpoint.cpu_reg;
  s_gb->counter = s_frame_checkpoint.counter;
  memcpy(s_gb->hram_io, s_frame_checkpoint.hram_io, sizeof(s_frame_checkpoint.hram_io));
  memcpy(s_gb->oam, s_frame_checkpoint.oam, sizeof(s_frame_checkpoint.oam));
  memcpy(s_gb->display.bg_palette, s_frame_checkpoint.bg_palette,
         sizeof(s_frame_checkpoint.bg_palette));
  memcpy(s_gb->display.sp_palette, s_frame_checkpoint.sp_palette,
         sizeof(s_frame_checkpoint.sp_palette));
  s_gb->display.window_clear = s_frame_checkpoint.window_clear;
  s_gb->display.WY = s_frame_checkpoint.WY;
  s_gb->display.frame_skip_count = s_frame_checkpoint.frame_skip_count;
  s_gb->display.interlace_count = s_frame_checkpoint.interlace_count;
}

static void prv_free_save_ram(void) {
  free(s_cart_ram);
  s_cart_ram = NULL;
  memset(&s_local_save, 0, sizeof(s_local_save));
  s_cart_ram_size = 0;
  s_cart_ram_window_size = 0;
  s_cart_ram_bank = CART_RAM_BANK_NONE;
  s_cart_ram_dirty = false;
  s_cart_ram_paused = false;
  s_cart_ram_faulted = false;
  s_cart_ram_loading = false;
  s_cart_ram_saving = false;
  s_cart_ram_loading_bank = CART_RAM_BANK_NONE;
  s_cart_ram_pending_bank = CART_RAM_BANK_NONE;
  s_cart_ram_loaded_bytes = 0;
  s_cart_ram_dirty_ms = 0;
  s_cart_ram_local = false;
}

static int prv_local_save_read(uint32_t key, void *data, size_t size, void *context) {
  (void)context;
  return persist_read_data(key, data, size);
}

static int prv_local_save_write(uint32_t key, const void *data, size_t size, void *context) {
  (void)context;
  return persist_write_data(key, data, size);
}

static void prv_flush_local_save(const char *reason) {
  if (!s_cart_ram_local || !s_cart_ram_dirty) {
    return;
  }
  uint64_t started_ms = prv_now_ms();
  int flushed = pb_save_flush_all(&s_local_save);
  s_cart_ram_dirty = pb_save_dirty_count(&s_local_save) != 0;
  APP_LOG(flushed < 0 ? APP_LOG_LEVEL_ERROR : APP_LOG_LEVEL_INFO,
          "local SRAM %s flush=%d dirty=%u writes=%u bytes=%lu elapsed=%lu",
          reason, flushed, (unsigned)pb_save_dirty_count(&s_local_save),
          (unsigned)s_local_save.write_ops, (unsigned long)s_local_save.write_bytes,
          (unsigned long)prv_elapsed_ms(started_ms));
}

static void prv_focus_handler(bool in_focus) {
  s_in_focus = in_focus;
  if (!in_focus) {
    pb_audio_suspend_stream();
    prv_flush_local_save("focus-loss");
    return;
  }

  s_next_frame_deadline_ms = prv_now_ms() + FRAME_MS;
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

  s_cart_ram = malloc(save_size);
  if (!s_cart_ram) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "local SRAM allocation failed: %u", (unsigned)save_size);
    prv_set_status("No SRAM heap");
    return false;
  }

  uint16_t rom_checksum = (uint16_t)((uint16_t)pb_cart_read(s_cart, 0x14E) << 8);
  rom_checksum |= pb_cart_read(s_cart, 0x14F);
  if (!pb_save_init(&s_local_save, s_cart_ram, save_size, rom_checksum,
                    prv_local_save_read, prv_local_save_write, NULL)) {
    free(s_cart_ram);
    s_cart_ram = NULL;
    prv_set_status("SRAM init failed");
    return false;
  }

  s_cart_ram_size = save_size;
  s_cart_ram_window_size = save_size;
  s_cart_ram_local = true;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "local SRAM ready: %u checksum=%04x restored=%u",
          (unsigned)save_size, (unsigned)rom_checksum,
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
  s_running = false;
  prv_free_save_ram();
  pb_video_clear(0);

  if (!pb_cart_has_bank(s_cart, 0)) {
    pb_cart_ensure_bank(s_cart, 0);
    prv_set_status("Loading bank 0");
    return false;
  }
  if (!pb_cart_ensure_fixed_bank(s_cart)) {
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
  pb_cart_set_active_bank(s_cart, s_gb->selected_rom_bank);

  gb_init_lcd(s_gb, prv_lcd_draw_line);
  s_gb->direct.frame_skip = true;
  s_gb->direct.interlace = false;

  size_t save_size = 0;
  if (gb_get_save_size_s(s_gb, &save_size) == 0 && save_size > 0) {
    if (s_cart->mode == PB_CART_MODE_PHONE) {
      s_cart_ram_size = save_size;
      s_cart_ram_window_size = save_size < CART_RAM_WINDOW_SIZE ? save_size : CART_RAM_WINDOW_SIZE;
      s_cart_ram_bank = CART_RAM_BANK_NONE;
      s_cart_ram_loading_bank = CART_RAM_BANK_NONE;
      s_cart_ram_pending_bank = CART_RAM_BANK_NONE;
      APP_LOG(APP_LOG_LEVEL_INFO,
              "phone SRAM window deferred: %u/%u bytes",
              (unsigned)s_cart_ram_window_size, (unsigned)save_size);
    } else if (!prv_init_local_save(save_size)) {
      return false;
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "using local persistent SRAM: %u",
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
  s_frame_budget_hits = 0;
  memset(&s_profile, 0, sizeof(s_profile));
  s_running = true;
  return true;
}

static bool prv_request_phone_bank(uint16_t bank, uint16_t offset, uint16_t size,
                                   bool demand, void *context) {
  (void)context;
  if (s_phone_bank_load_pending) {
    return false;
  }
  uint64_t started_ms = prv_now_ms();
  if (gb_phone_request_bank(bank, offset, size)) {
    s_phone_bank_load_pending = true;
    s_phone_bank_load_bank = bank;
    s_phone_bank_load_offset = offset;
    s_phone_bank_load_size = size;
    s_phone_bank_load_demand = demand;
    s_phone_bank_load_ms = started_ms;
    if (demand) {
      snprintf(s_status, sizeof(s_status), "Loading bank %u", bank);
      layer_mark_dirty(s_canvas);
    }
    return true;
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "phone request busy for bank %u fill %u",
          (unsigned)bank, (unsigned)offset);
  return false;
}

void pb_core_rom_bank_changed(struct gb_s *gb) {
  if (gb == s_gb && s_cart) {
    pb_cart_set_active_bank(s_cart, gb->selected_rom_bank);
  }
}

bool pb_core_should_pause(struct gb_s *gb) {
  return gb == s_gb && ((s_cart && pb_cart_paused(s_cart)) || s_cart_ram_paused);
}

static void prv_resume_cart_ram(void) {
  s_cart_ram_paused = false;
  s_cart_ram_faulted = false;
  s_cart_ram_pending_bank = CART_RAM_BANK_NONE;
  prv_set_status("Resumed");
  prv_schedule_frame_timer(1);
}

static void prv_maybe_continue_cart_ram(void) {
  if (!s_cart_ram_paused || s_cart_ram_loading || s_cart_ram_saving ||
      s_cart_ram_pending_bank == CART_RAM_BANK_NONE) {
    return;
  }

  uint16_t pending_bank = s_cart_ram_pending_bank;
  if (pending_bank == s_cart_ram_bank) {
    prv_resume_cart_ram();
    return;
  }
  if (s_cart_ram_dirty && s_cart_ram_bank != CART_RAM_BANK_NONE) {
    prv_start_cart_ram_save(pending_bank);
  } else {
    prv_start_cart_ram_load(pending_bank);
  }
}

static void prv_maybe_flush_cart_ram(uint64_t now) {
  /* Local persistence is synchronous physical-flash I/O and can block an app
   * tick for over 100 ms. Flush it on focus loss/shutdown instead; phone SRAM
   * remains asynchronous and can retain its normal settle timer. */
  if (s_cart_ram_local) {
    return;
  }
  if (!s_cart_ram || !s_cart_ram_dirty || s_cart_ram_bank == CART_RAM_BANK_NONE ||
      s_cart_ram_paused || s_cart_ram_loading || s_cart_ram_saving) {
    return;
  }
  if (now - s_cart_ram_dirty_ms < CART_RAM_SAVE_SETTLE_MS) {
    return;
  }

  s_cart_ram_faulted = false;
  if (!prv_start_cart_ram_save(s_cart_ram_bank)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "phone SRAM save busy for bank %u",
            (unsigned)s_cart_ram_bank);
  }
}

static void prv_maybe_prefetch_next_phone_fill(const PbPhoneEvent *event, bool after_demand) {
  if (!after_demand || !s_running || !s_cart || s_cart->mode != PB_CART_MODE_PHONE ||
      pb_cart_paused(s_cart) || s_cart_ram_paused || s_phone_bank_load_pending) {
    return;
  }
  if (event->bank != s_cart->active_bank || event->size >= PB_CART_BANK_SIZE) {
    return;
  }

  uint32_t next_offset = (uint32_t)event->offset + event->size;
  if (next_offset >= PB_CART_BANK_SIZE) {
    return;
  }
  uint32_t addr = (uint32_t)event->bank * PB_CART_BANK_SIZE + next_offset;
  pb_cart_prefetch_addr(s_cart, addr);
}

static PB_SIZE_OPT void prv_phone_event(const PbPhoneEvent *event, void *context) {
  (void)context;
  switch (event->type) {
    case PB_PHONE_EVENT_ROM_LIST_BEGIN:
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
      s_rom_count = (uint8_t)(event->size > ROM_LIBRARY_MAX
                                  ? ROM_LIBRARY_MAX
                                  : event->size);
      s_phone_library_seen = true;
      if (s_rom_count == 0) {
        s_rom_selection_pending = false;
        prv_set_status("Add ROMs in app settings");
      } else if (s_rom_count == 1) {
        s_rom_selection_pending = true;
        s_rom_selected = 0;
        prv_set_status("Loading ROM");
        gb_phone_select_rom(0);
        s_last_phone_info_request_ms = prv_now_ms();
      } else {
        s_rom_selection_pending = false;
        prv_set_rom_selector(true);
      }
      if (s_canvas) {
        layer_mark_dirty(s_canvas);
      }
      break;
    case PB_PHONE_EVENT_INFO:
      if (s_cart && s_cart->mode == PB_CART_MODE_PHONE) {
        APP_LOG(APP_LOG_LEVEL_INFO, "ignoring duplicate phone ROM info");
        break;
      }
      s_phone_offer_seen = true;
      s_rom_selection_pending = false;
      prv_set_rom_selector(false);
      s_running = false;
      prv_free_save_ram();
      APP_LOG(APP_LOG_LEVEL_INFO, "phone info title=%s size=%lu cart=%u scale=%u",
              event->title[0] ? event->title : "DMG ROM", event->size,
              (unsigned)event->cart_type, (unsigned)event->video_scale);
      pb_video_set_scale(event->video_scale <= PB_VIDEO_SCALE_ASPECT_FIT
                             ? (PbVideoScale)event->video_scale
                             : PB_VIDEO_SCALE_1X);
      prv_update_canvas_frame();
      pb_audio_set_enabled(event->audio_enabled);
      if (pb_cart_init_phone(s_cart, event->size, prv_request_phone_bank, NULL)) {
        snprintf(s_status, sizeof(s_status), "Phone ROM %s",
                 event->title[0] ? event->title : "loading");
        pb_cart_ensure_bank(s_cart, 0);
      }
      layer_mark_dirty(s_canvas);
      break;
    case PB_PHONE_EVENT_BANK_READY:
      uint32_t latency_ms = 0;
      bool completed_demand = false;
      if (s_phone_bank_load_pending && s_phone_bank_load_bank == event->bank &&
          s_phone_bank_load_offset == event->offset && s_phone_bank_load_size == event->size) {
        latency_ms = prv_elapsed_ms(s_phone_bank_load_ms);
        completed_demand = s_phone_bank_load_demand;
        s_phone_bank_load_pending = false;
        s_phone_bank_load_demand = false;
        s_cart->stats.last_load_ms = latency_ms;
      }
      APP_LOG(APP_LOG_LEVEL_INFO, "phone bank %u ready fill=%u size=%u latency_ms=%lu",
              (unsigned)event->bank, (unsigned)event->offset,
              (unsigned)event->size, latency_ms);
      if (!s_running && event->bank == 0) {
        prv_start_from_cart("phone");
      } else if (s_cart && s_cart->mode == PB_CART_MODE_PHONE && !pb_cart_paused(s_cart)) {
        prv_set_status("Resumed");
        prv_schedule_frame_timer(1);
      }
      prv_maybe_prefetch_next_phone_fill(event, completed_demand);
      break;
    case PB_PHONE_EVENT_SRAM_LOAD_DATA:
      if (s_cart_ram && s_cart_ram_loading && event->bank == s_cart_ram_loading_bank &&
          event->offset < s_cart_ram_window_size) {
        uint16_t len = event->data_len;
        if (event->offset + len > s_cart_ram_window_size) {
          len = (uint16_t)(s_cart_ram_window_size - event->offset);
        }
        memcpy(s_cart_ram + event->offset, event->data, len);
        if (event->offset + len > s_cart_ram_loaded_bytes) {
          s_cart_ram_loaded_bytes = event->offset + len;
        }
        if (s_cart_ram_loaded_bytes >= s_cart_ram_window_size) {
          s_cart_ram_bank = s_cart_ram_loading_bank;
          s_cart_ram_loading = false;
          s_cart_ram_loading_bank = CART_RAM_BANK_NONE;
          s_cart_ram_loaded_bytes = 0;
          s_cart_ram_dirty = false;
          APP_LOG(APP_LOG_LEVEL_INFO, "phone SRAM bank %u loaded",
                  (unsigned)s_cart_ram_bank);
          prv_resume_cart_ram();
        }
      }
      break;
    case PB_PHONE_EVENT_SRAM_SAVE_DONE:
      if (s_cart_ram_saving && event->bank == s_cart_ram_bank) {
        s_cart_ram_saving = false;
        s_cart_ram_dirty = false;
        APP_LOG(APP_LOG_LEVEL_INFO, "phone SRAM bank %u saved",
                (unsigned)event->bank);
        if (s_cart_ram_pending_bank != CART_RAM_BANK_NONE &&
            s_cart_ram_pending_bank != s_cart_ram_bank) {
          uint16_t pending_bank = s_cart_ram_pending_bank;
          if (!prv_start_cart_ram_load(pending_bank)) {
            s_cart_ram_paused = true;
            s_cart_ram_pending_bank = pending_bank;
            prv_cart_ram_show_status("Loading", pending_bank);
          }
        } else {
          prv_resume_cart_ram();
        }
      }
      break;
    case PB_PHONE_EVENT_ERROR:
      if (s_cart && s_cart->mode == PB_CART_MODE_PHONE) {
        pb_cart_set_error(s_cart, event->status);
      }
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
          "fps=%u cache m=%lu fills=%lu req=%lu last=%u/%u load_ms=%lu heap=%u/%u",
          (unsigned)((frame_delta * 1000u) / (ms_delta ? ms_delta : 1)),
          stats->misses, stats->loads, stats->phone_requests,
          (unsigned)stats->last_miss_bank, (unsigned)stats->last_load_bank,
          stats->last_load_ms, (unsigned)heap_bytes_free(), (unsigned)heap_bytes_used());
  APP_LOG(APP_LOG_LEVEL_INFO,
          "io reads=%lu bytes=%lu save dirty=%u reads=%u/%lu writes=%u/%lu audio=%lu/%lu/%lu",
          stats->source_reads, stats->source_bytes,
          (unsigned)(s_cart_ram_local ? pb_save_dirty_count(&s_local_save) : 0),
          (unsigned)(s_cart_ram_local ? s_local_save.read_ops : 0),
          (unsigned long)(s_cart_ram_local ? s_local_save.read_bytes : 0),
          (unsigned)(s_cart_ram_local ? s_local_save.write_ops : 0),
          (unsigned long)(s_cart_ram_local ? s_local_save.write_bytes : 0),
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
  if (s_phone_offer_seen || (s_cart && s_cart->mode != PB_CART_MODE_NONE)) {
    return;
  }
  if (now - s_last_phone_info_request_ms < PHONE_INFO_RETRY_MS) {
    return;
  }
  if (!s_phone_library_seen) {
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

static bool prv_rom_addr_for_pc(uint16_t pc, uint32_t *addr) {
  if (pc < 0x4000) {
    *addr = pc;
    return true;
  }
  if (pc < 0x8000) {
    uint16_t bank = s_gb->selected_rom_bank ? s_gb->selected_rom_bank : 1;
    *addr = (uint32_t)pc + ((uint32_t)bank - 1u) * PB_CART_BANK_SIZE;
    return true;
  }
  return false;
}

static bool prv_ensure_cpu_fetch_window(void) {
  for (uint16_t ahead = 0; ahead < 3; ahead++) {
    uint32_t addr;
    uint16_t pc = (uint16_t)(s_gb->cpu_reg.pc.reg + ahead);
    if (!prv_rom_addr_for_pc(pc, &addr)) {
      continue;
    }
    if (!pb_cart_ensure_addr(s_cart, addr)) {
      return false;
    }
  }
  return true;
}

static bool prv_run_one_frame(uint32_t *step_budget) {
  s_gb->direct.joypad = pb_input_joypad();
  s_gb->gb_frame = false;

  if (s_cart->mode == PB_CART_MODE_RESOURCE) {
    while (!s_gb->gb_frame) {
      __gb_step_cpu(s_gb);
    }
    if (s_cart->failed) {
      return false;
    }
    s_frames++;
    return true;
  }

  while (!s_gb->gb_frame && *step_budget) {
    if (pb_cart_paused(s_cart) || s_cart_ram_paused || !prv_ensure_cpu_fetch_window()) {
      return false;
    }
    pb_cart_clear_read_fault(s_cart);
    s_cart_ram_faulted = false;
    prv_save_frame_checkpoint();
    *step_budget -= 1;
    __gb_step_cpu(s_gb);
    if (pb_cart_read_faulted(s_cart) || pb_cart_paused(s_cart) || s_cart_ram_faulted) {
      prv_restore_frame_checkpoint();
      return false;
    }
  }

  if (!s_gb->gb_frame) {
    s_frame_budget_hits++;
    if ((s_frame_budget_hits & 0x1F) == 1) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "frame step budget hit pc=%04x",
              (unsigned)s_gb->cpu_reg.pc.reg);
    }
    return false;
  }

  s_frames++;
  return true;
}

static void prv_frame_timer_cb(void *data) {
  (void)data;
  s_timer = NULL;
  uint64_t now = prv_now_ms();
  uint64_t tick_start_ms = now;
  prv_maybe_request_phone_info(now);
  prv_maybe_continue_cart_ram();

  if (!s_running && s_cart && s_cart->mode == PB_CART_MODE_PHONE && !pb_cart_paused(s_cart)) {
    prv_start_from_cart("phone");
  }

  bool can_run = s_running && s_in_focus && !pb_cart_paused(s_cart) &&
                 !s_cart_ram_paused;
  if (can_run) {
    uint32_t step_budget = MAX_CPU_STEPS_PER_TICK;
    uint8_t completed_frames = 0;
    for (int i = 0; i < FRAMES_PER_TICK && step_budget; i++) {
      uint64_t phase_start_ms = prv_now_ms();
      if (!prv_run_one_frame(&step_budget)) {
        s_profile.cpu_ms += prv_elapsed_ms(phase_start_ms);
        break;
      }
      s_profile.cpu_ms += prv_elapsed_ms(phase_start_ms);
      completed_frames++;

      // Capture APU state after each emulated frame. Pumping only after both
      // frames discarded every other 60 Hz register update and made music
      // transitions sound coarse even when the PCM stream itself was healthy.
      phase_start_ms = prv_now_ms();
      pb_audio_pump();
      s_profile.audio_ms += prv_elapsed_ms(phase_start_ms);
    }

    while (completed_frames < FRAMES_PER_TICK) {
      uint64_t phase_start_ms = prv_now_ms();
      pb_audio_pump_silence();
      s_profile.audio_ms += prv_elapsed_ms(phase_start_ms);
      completed_frames++;
    }
    uint64_t phase_start_ms = prv_now_ms();
    prv_maybe_flush_cart_ram(prv_now_ms());
    s_profile.save_ms += prv_elapsed_ms(phase_start_ms);
  } else {
    if (s_running && s_in_focus) {
      pb_audio_pump_silence();
    } else {
      pb_audio_suspend_stream();
    }
  }

  if (s_canvas && pb_video_take_changed()) {
    layer_mark_dirty(s_canvas);
    s_profile.presented++;
  } else if (s_running) {
    s_profile.unchanged++;
  }

  uint64_t tick_end_ms = prv_now_ms();
  uint64_t elapsed = tick_end_ms - tick_start_ms;
  if (s_running) {
    uint32_t elapsed_ms = (uint32_t)elapsed;
    s_profile.tick_ms += elapsed_ms;
    if (elapsed_ms > s_profile.max_tick_ms) {
      s_profile.max_tick_ms = elapsed_ms;
    }
    s_profile.ticks++;
    prv_log_perf();
  }
  if (!s_next_frame_deadline_ms) {
    s_next_frame_deadline_ms = tick_start_ms;
  }
  s_next_frame_deadline_ms += FRAME_MS;
  if (tick_end_ms > s_next_frame_deadline_ms + FRAME_CATCHUP_LIMIT_MS) {
    s_next_frame_deadline_ms = tick_end_ms + FRAME_MS;
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

  GRect root_frame = layer_get_frame(window_get_root_layer(s_window));
  PbVideoScale scale = pb_video_scale();
  GRect render_bounds = scale == PB_VIDEO_SCALE_1X
                            ? layer_get_frame(layer)
                            : root_frame;
  if (scale == PB_VIDEO_SCALE_1X) {
    render_bounds.origin.x += root_frame.origin.x;
    render_bounds.origin.y += root_frame.origin.y;
  }
  pb_video_render(ctx, render_bounds);

  if (!s_running || !s_cart || pb_cart_paused(s_cart) || s_cart_ram_paused) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_status, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(4, bounds.size.h - 34, bounds.size.w - 8, 30),
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
  if (!s_rom_selector || s_rom_selected >= s_rom_count) {
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

  pb_video_init();
  prv_update_canvas_frame();
  pb_input_init();
  pb_audio_init();
  s_in_focus = true;
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

  if (!started_local) {
    prv_set_status("Loading ROM library");
    gb_phone_request_rom_list();
    s_last_phone_info_request_ms = prv_now_ms();
  }

#ifdef PBL_TOUCH
  touch_service_subscribe(prv_touch_handler, NULL);
#endif

  s_next_frame_deadline_ms = prv_now_ms() + FRAME_MS;
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
  app_focus_service_unsubscribe();
  gb_phone_deinit();
  pb_audio_deinit();
  prv_flush_local_save("shutdown");
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
