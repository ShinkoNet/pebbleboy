#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gb_cart.h"

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static uint8_t line_pattern(uint16_t bank, uint16_t line) {
  return (uint8_t)(0x40u | ((bank & 0x0Fu) << 2) | (line & 0x03u));
}

int main(void) {
  const uint16_t bank_count = 8;
  const size_t rom_size = (size_t)bank_count * PB_CART_BANK_SIZE;
  uint8_t *rom = malloc(rom_size);
  expect(rom != NULL, "ROM allocation failed");

  for (uint16_t bank = 0; bank < bank_count; bank++) {
    for (uint16_t line = 0; line < PB_CART_BANK_SIZE / PB_CART_LINE_SIZE; line++) {
      size_t start = (size_t)bank * PB_CART_BANK_SIZE + (size_t)line * PB_CART_LINE_SIZE;
      for (size_t i = 0; i < PB_CART_LINE_SIZE; i++) {
        rom[start + i] = line_pattern(bank, line);
      }
    }
  }

  PbCart cart;
  expect(pb_cart_init_memory(&cart, rom, (uint32_t)rom_size), "cart init failed");
  expect(cart.bank_count == bank_count, "unexpected bank count");

  pb_cart_set_active_bank(&cart, 2);
  for (uint16_t line = 0; line < PB_CART_BANK_SIZE / PB_CART_LINE_SIZE; line++) {
    uint32_t addr = 2u * PB_CART_BANK_SIZE + (uint32_t)line * PB_CART_LINE_SIZE;
    expect(pb_cart_ensure_addr(&cart, addr), "active bank line load failed");
  }

  for (uint16_t bank = 3; bank <= 5; bank++) {
    for (uint16_t line = 0; line < PB_CART_BANK_SIZE / PB_CART_LINE_SIZE; line++) {
      uint32_t addr = (uint32_t)bank * PB_CART_BANK_SIZE + (uint32_t)line * PB_CART_LINE_SIZE;
      expect(pb_cart_ensure_addr(&cart, addr), "inactive bank line load failed");
    }
  }

  const uint32_t loads_before_probe = pb_cart_stats(&cart)->loads;
  for (uint16_t line = 0; line < PB_CART_BANK_SIZE / PB_CART_LINE_SIZE; line++) {
    uint32_t addr = 2u * PB_CART_BANK_SIZE + (uint32_t)line * PB_CART_LINE_SIZE;
    uint8_t value = pb_cart_read(&cart, addr);
    expect(value == line_pattern(2, line), "active bank returned wrong byte");
  }
  expect(pb_cart_stats(&cart)->loads == loads_before_probe, "active bank line was evicted");

  free(rom);
  printf("cart cache active-bank test passed loads=%lu hits=%lu misses=%lu\n",
         (unsigned long)pb_cart_stats(&cart)->loads,
         (unsigned long)pb_cart_stats(&cart)->hits,
         (unsigned long)pb_cart_stats(&cart)->misses);
  return 0;
}
