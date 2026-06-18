#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gb_cart.h"

#define PROFILE_BANK_COUNT 4u
#define PROFILE_SWITCH_BANKS 3u
#define PROFILE_LINES_PER_BANK (PB_CART_BANK_SIZE / PB_CART_LINE_SIZE)

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static uint8_t pattern_for(uint16_t bank, uint16_t line) {
  return (uint8_t)(0x80u | ((bank & 0x0Fu) << 2) | (line & 0x03u));
}

static void read_switch_bank(PbCart *cart, uint8_t *rom, uint16_t bank) {
  pb_cart_set_active_bank(cart, bank);
  for (uint16_t line = 0; line < PROFILE_LINES_PER_BANK; line++) {
    uint32_t addr = (uint32_t)bank * PB_CART_BANK_SIZE +
                    (uint32_t)line * PB_CART_LINE_SIZE;
    uint8_t got = pb_cart_read(cart, addr);
    uint8_t expected = pattern_for(bank, line);
    if (got != expected || got != rom[addr]) {
      fprintf(stderr, "bank %u line %u read %02x expected %02x\n",
              (unsigned)bank, (unsigned)line, got, expected);
      exit(1);
    }
  }
}

int main(void) {
  const size_t rom_size = PROFILE_BANK_COUNT * PB_CART_BANK_SIZE;
  uint8_t *rom = malloc(rom_size);
  expect(rom != NULL, "ROM allocation failed");

  for (uint16_t bank = 0; bank < PROFILE_BANK_COUNT; bank++) {
    for (uint16_t line = 0; line < PROFILE_LINES_PER_BANK; line++) {
      size_t start = (size_t)bank * PB_CART_BANK_SIZE + (size_t)line * PB_CART_LINE_SIZE;
      uint8_t value = pattern_for(bank, line);
      for (size_t i = 0; i < PB_CART_LINE_SIZE; i++) {
        rom[start + i] = value;
      }
    }
  }

  PbCart cart;
  expect(pb_cart_init_memory(&cart, rom, (uint32_t)rom_size), "cart init failed");
  expect(pb_cart_ensure_fixed_bank(&cart), "fixed bank preload failed");

  const uint32_t baseline_loads = pb_cart_stats(&cart)->loads;
  const uint32_t baseline_misses = pb_cart_stats(&cart)->misses;

  for (uint16_t bank = 1; bank <= PROFILE_SWITCH_BANKS; bank++) {
    read_switch_bank(&cart, rom, bank);
  }
  const uint32_t first_pass_loads = pb_cart_stats(&cart)->loads - baseline_loads;
  const uint32_t first_pass_misses = pb_cart_stats(&cart)->misses - baseline_misses;

  const uint32_t after_first_loads = pb_cart_stats(&cart)->loads;
  const uint32_t after_first_misses = pb_cart_stats(&cart)->misses;
  for (uint16_t bank = 1; bank <= PROFILE_SWITCH_BANKS; bank++) {
    read_switch_bank(&cart, rom, bank);
  }
  const uint32_t second_pass_loads = pb_cart_stats(&cart)->loads - after_first_loads;
  const uint32_t second_pass_misses = pb_cart_stats(&cart)->misses - after_first_misses;
  const uint32_t expected_first = PROFILE_SWITCH_BANKS * PROFILE_LINES_PER_BANK;

  expect(first_pass_loads == expected_first, "first cache profile pass load count mismatch");
  expect(first_pass_misses == expected_first, "first cache profile pass miss count mismatch");
#if PB_CART_CACHE_BANKS >= 4
  expect(second_pass_loads == 0, "4-bank cache reloaded the working set");
  expect(second_pass_misses == 0, "4-bank cache missed the retained working set");
#elif PB_CART_CACHE_BANKS == 3
  expect(second_pass_loads == expected_first, "3-bank cache did not show expected reload pressure");
  expect(second_pass_misses == expected_first, "3-bank cache did not show expected miss pressure");
#else
  expect(second_pass_loads >= expected_first, "small cache profile unexpectedly retained the working set");
#endif

  printf("cache profile banks=%u slots=%u first_loads=%u second_loads=%u "
         "first_misses=%u second_misses=%u\n",
         (unsigned)PB_CART_CACHE_BANKS, (unsigned)PB_CART_CACHE_SLOTS,
         (unsigned)first_pass_loads, (unsigned)second_pass_loads,
         (unsigned)first_pass_misses, (unsigned)second_pass_misses);

  free(rom);
  return 0;
}
