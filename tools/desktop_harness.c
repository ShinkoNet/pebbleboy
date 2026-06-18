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
  (void)gb;
  pb_video_draw_line(pixels, (uint8_t)line);
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

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s ROM [frames]\n", argv[0]);
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

  pb_video_init();
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
  gb_init_lcd(&s_gb, lcd_line);

  if (gb_get_save_size_s(&s_gb, &s_save_ram_size) == 0 && s_save_ram_size) {
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

  char title[17];
  gb_get_rom_name(&s_gb, title);
  const PbCartStats *stats = pb_cart_stats(&s_cart);
  printf("rom=%s title=\"%s\" frames=%d mbc=%d banks=%u save=%zu hash=%08x hits=%u misses=%u loads=%u\n",
         argv[1], title, frames, (int)s_gb.mbc, (unsigned)s_cart.bank_count,
         s_save_ram_size, pb_video_hash(), (unsigned)stats->hits,
         (unsigned)stats->misses, (unsigned)stats->loads);

  free(s_save_ram);
  free(rom);
  return 0;
}
