#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_cart.h"
#include "gb_core.h"
#include "gb_video.h"

static PbCart s_cart;
static struct gb_s s_gb;
static uint8_t *s_save_ram;
static size_t s_save_ram_size;
static char s_serial[8192];
static size_t s_serial_size;

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  return pb_cart_read((PbCart *)gb->direct.priv, (uint32_t)addr);
}

static uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  return (addr < s_save_ram_size && s_save_ram) ? s_save_ram[addr] : 0xFF;
}

static void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
  (void)gb;
  if (addr < s_save_ram_size && s_save_ram) {
    s_save_ram[addr] = val;
  }
}

static void gb_error(struct gb_s *gb, const enum gb_error_e error, const uint16_t addr) {
  (void)gb;
  fprintf(stderr, "core error %d at %04x\n", (int)error, addr);
  exit(2);
}

static void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
  if (gb->cgb.cgbMode) {
    pb_video_draw_line_cgb(pixels, gb->cgb.fixPalette, (uint8_t)line);
  } else {
    pb_video_draw_line(pixels, (uint8_t)line);
  }
}

static void serial_tx(struct gb_s *gb, const uint8_t value) {
  (void)gb;
  if (s_serial_size + 1 < sizeof(s_serial)) {
    s_serial[s_serial_size++] = (char)value;
    s_serial[s_serial_size] = '\0';
  }
}

void pb_core_rom_bank_changed(struct gb_s *gb) {
  if (gb == &s_gb) {
    pb_cart_set_active_bank(&s_cart, gb->selected_rom_bank);
  }
}

static void format_bank_mask(uint64_t mask, char *out, size_t out_size) {
  size_t pos = 0;
  bool first = true;
  if (out_size == 0) {
    return;
  }
  out[0] = '\0';
  for (int bank = 0; bank < 64; bank++) {
    if (!(mask & (((uint64_t)1) << bank))) {
      continue;
    }
    int written = snprintf(out + pos, out_size - pos, "%s%d", first ? "" : ",", bank);
    if (written < 0 || (size_t)written >= out_size - pos) {
      out[out_size - 1] = '\0';
      return;
    }
    pos += (size_t)written;
    first = false;
  }
  if (first) {
    snprintf(out, out_size, "none");
  }
}

static uint8_t *read_file(const char *path, size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  *size_out = (size_t)size;
  return buf;
}

static void write_le16(FILE *f, uint16_t value) {
  fputc(value & 0xFF, f);
  fputc((value >> 8) & 0xFF, f);
}

static void write_le32(FILE *f, uint32_t value) {
  fputc(value & 0xFF, f);
  fputc((value >> 8) & 0xFF, f);
  fputc((value >> 16) & 0xFF, f);
  fputc((value >> 24) & 0xFF, f);
}

static bool write_bmp(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return false;
  }
  const uint32_t row_size = PB_GB_LCD_W * 3;
  const uint32_t image_size = row_size * PB_GB_LCD_H;
  const uint32_t pixel_offset = 14 + 40;
  const uint32_t file_size = pixel_offset + image_size;

  fputc('B', f);
  fputc('M', f);
  write_le32(f, file_size);
  write_le16(f, 0);
  write_le16(f, 0);
  write_le32(f, pixel_offset);

  write_le32(f, 40);
  write_le32(f, PB_GB_LCD_W);
  write_le32(f, PB_GB_LCD_H);
  write_le16(f, 1);
  write_le16(f, 24);
  write_le32(f, 0);
  write_le32(f, image_size);
  write_le32(f, 2835);
  write_le32(f, 2835);
  write_le32(f, 0);
  write_le32(f, 0);

  for (int y = PB_GB_LCD_H - 1; y >= 0; y--) {
    for (uint8_t x = 0; x < PB_GB_LCD_W; x++) {
      uint8_t pixel = pb_video_get_pixel(x, (uint8_t)y);
      fputc((pixel & 3u) * 85u, f);
      fputc(((pixel >> 2) & 3u) * 85u, f);
      fputc(((pixel >> 4) & 3u) * 85u, f);
    }
  }
  fclose(f);
  return true;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s ROM [frames] [frame.bmp]\n", argv[0]);
    return 1;
  }
  int frames = argc >= 3 ? atoi(argv[2]) : 120;
  if (frames <= 0) {
    frames = 120;
  }

  size_t rom_size = 0;
  uint8_t *rom = read_file(argv[1], &rom_size);
  if (!rom) {
    return 1;
  }

  if (!pb_video_init()) {
    fprintf(stderr, "video allocation failed\n");
    free(rom);
    return 1;
  }
  if (!pb_cart_init_memory(&s_cart, rom, (uint32_t)rom_size)) {
    fprintf(stderr, "cart init failed: %s\n", s_cart.error);
    free(rom);
    return 1;
  }

  enum gb_init_error_e err = gb_init(&s_gb, rom_read, ram_read, ram_write, gb_error, &s_cart);
  if (err != GB_INIT_NO_ERROR) {
    fprintf(stderr, "gb_init failed: %d\n", (int)err);
    free(rom);
    return 1;
  }
  pb_cart_set_active_bank(&s_cart, s_gb.selected_rom_bank);
  gb_init_lcd(&s_gb, lcd_line);
  if (getenv("PB_DESKTOP_SERIAL")) {
    gb_init_serial(&s_gb, serial_tx, NULL);
  }

  if (getenv("PB_DESKTOP_NO_SAVE")) {
    s_save_ram_size = 0;
  } else if (gb_get_save_size_s(&s_gb, &s_save_ram_size) == 0 && s_save_ram_size) {
    s_save_ram = malloc(s_save_ram_size);
    if (!s_save_ram) {
      fprintf(stderr, "save RAM alloc failed: %zu\n", s_save_ram_size);
      free(rom);
      return 1;
    }
    memset(s_save_ram, 0xFF, s_save_ram_size);
  }

  for (int i = 0; i < frames; i++) {
    gb_run_frame(&s_gb);
  }

  if (argc >= 4 && !write_bmp(argv[3])) {
    free(s_save_ram);
    free(rom);
    return 1;
  }

  char title[17];
  gb_get_rom_name(&s_gb, title);
  const PbCartStats *stats = pb_cart_stats(&s_cart);
  char load_banks[192];
  char request_banks[192];
  format_bank_mask(stats->load_bank_mask, load_banks, sizeof(load_banks));
  format_bank_mask(stats->request_bank_mask, request_banks, sizeof(request_banks));
  uint8_t save0 = (s_save_ram && s_save_ram_size) ? s_save_ram[0] : 0xFF;
  size_t save_nonff = 0;
  for (size_t i = 0; i < s_save_ram_size && s_save_ram; i++) {
    if (s_save_ram[i] != 0xFF) {
      save_nonff++;
    }
  }
  uint64_t colour_mask = 0;
  for (uint8_t y = 0; y < PB_GB_LCD_H; y++) {
    for (uint8_t x = 0; x < PB_GB_LCD_W; x++) {
      colour_mask |= ((uint64_t)1) << pb_video_get_pixel(x, y);
    }
  }
  unsigned colour_count = 0;
  for (uint8_t colour = 0; colour < 64; colour++) {
    colour_count += (unsigned)((colour_mask >> colour) & 1u);
  }
  printf("rom=%s title=\"%s\" frames=%d cgb=%u mbc=%d banks=%u save=%zu save0=%02x save_nonff=%zu hash=%08x colors=%u hits=%u misses=%u loads=%u source_reads=%u source_bytes=%u load_banks=%s request_banks=%s\n",
         argv[1], title, frames, (unsigned)s_gb.cgb.cgbMode, (int)s_gb.mbc,
         (unsigned)s_cart.bank_count, s_save_ram_size, save0, save_nonff,
         pb_video_hash(), colour_count,
         (unsigned)stats->hits, (unsigned)stats->misses, (unsigned)stats->loads,
         (unsigned)stats->source_reads, (unsigned)stats->source_bytes,
         load_banks, request_banks);
  printf("cpu pc=%04x af=%04x bc=%04x de=%04x hl=%04x sp=%04x\n",
         s_gb.cpu_reg.pc.reg,
         (unsigned)((uint16_t)s_gb.cpu_reg.a << 8 |
                    ((uint16_t)s_gb.cpu_reg.f_bits.z << 7) |
                    ((uint16_t)s_gb.cpu_reg.f_bits.n << 6) |
                    ((uint16_t)s_gb.cpu_reg.f_bits.h << 5) |
                    ((uint16_t)s_gb.cpu_reg.f_bits.c << 4)),
         s_gb.cpu_reg.bc.reg, s_gb.cpu_reg.de.reg, s_gb.cpu_reg.hl.reg,
         s_gb.cpu_reg.sp.reg);
  if (s_serial_size) {
    printf("serial:\n%.*s\n", (int)s_serial_size, s_serial);
  }

  free(s_save_ram);
  pb_video_deinit();
  free(rom);
  return 0;
}
