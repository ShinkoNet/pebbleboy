#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_cart.h"

typedef struct {
  uint16_t bank;
  uint16_t offset;
  uint16_t size;
  bool demand;
  unsigned count;
} RequestCapture;

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static bool capture_request(uint16_t bank, uint16_t offset, uint16_t size,
                            bool demand, void *context) {
  RequestCapture *capture = context;
  capture->bank = bank;
  capture->offset = offset;
  capture->size = size;
  capture->demand = demand;
  capture->count++;
  return true;
}

static uint8_t line_pattern(uint16_t bank, uint16_t line) {
  return (uint8_t)(0x40u | ((bank & 0x0Fu) << 2) | (line & 0x03u));
}

static void test_active_bank_retention(void) {
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
}

static void test_phone_prefetch(void) {
  PbCart cart;
  RequestCapture capture = {0};
  expect(pb_cart_init_phone(&cart, 8u * PB_CART_BANK_SIZE, capture_request, &capture),
         "phone cart init failed");

  uint32_t addr = PB_CART_BANK_SIZE + PB_CART_LINE_SIZE;
  expect(pb_cart_prefetch_addr(&cart, addr), "phone prefetch did not start");
  expect(capture.count == 1, "prefetch request count mismatch");
  expect(capture.bank == 1 && capture.offset == PB_CART_LINE_SIZE &&
         capture.size == PB_CART_LINE_SIZE, "prefetch request range mismatch");
  expect(!capture.demand, "prefetch was marked as demand");
  expect(!pb_cart_paused(&cart), "prefetch paused the cart");

  expect(!pb_cart_ensure_addr(&cart, addr), "loading prefetch unexpectedly satisfied demand");
  expect(pb_cart_paused(&cart), "demand for loading prefetch did not pause");
  expect(capture.count == 1, "loading prefetch issued duplicate demand request");

  uint8_t *line = malloc(PB_CART_LINE_SIZE);
  expect(line != NULL, "prefetch line allocation failed");
  memset(line, 0x5A, PB_CART_LINE_SIZE);
  expect(pb_cart_phone_begin(&cart, capture.bank, capture.offset, capture.size),
         "prefetch begin failed");
  expect(pb_cart_phone_data(&cart, capture.bank, capture.offset, line, capture.size),
         "prefetch data failed");
  expect(pb_cart_phone_end(&cart, capture.bank, capture.offset, capture.size),
         "prefetch end failed");
  expect(!pb_cart_paused(&cart), "prefetch completion did not resume demand");
  expect(pb_cart_read(&cart, addr) == 0x5A, "prefetched line readback failed");
  free(line);
  printf("cart cache phone-prefetch test passed requests=%u\n", capture.count);
}

static void test_phone_fixed_bank_fill_size(void) {
  PbCart cart;
  RequestCapture capture = {0};
  expect(pb_cart_init_phone(&cart, 8u * PB_CART_BANK_SIZE, capture_request, &capture),
         "phone cart init failed");

  expect(!pb_cart_ensure_bank(&cart, 0), "phone fixed bank was unexpectedly present");
  expect(capture.count == 1, "fixed bank request count mismatch");
  expect(capture.bank == 0 && capture.offset == 0 && capture.size == PB_CART_BANK_SIZE,
         "fixed bank did not request one full 16 KiB fill");
  expect(capture.demand, "fixed bank request was not marked as demand");

  uint8_t *bank0 = malloc(PB_CART_BANK_SIZE);
  expect(bank0 != NULL, "fixed bank buffer allocation failed");
  memset(bank0, 0xA5, PB_CART_BANK_SIZE);
  expect(pb_cart_phone_begin(&cart, 0, 0, PB_CART_BANK_SIZE), "fixed bank begin failed");
  expect(pb_cart_phone_data(&cart, 0, 0, bank0, PB_CART_BANK_SIZE),
         "fixed bank data failed");
  expect(pb_cart_phone_end(&cart, 0, 0, PB_CART_BANK_SIZE), "fixed bank end failed");
  expect(pb_cart_read(&cart, 0) == 0xA5, "fixed bank readback failed");
  free(bank0);

  memset(&capture, 0, sizeof(capture));
  expect(!pb_cart_ensure_addr(&cart, PB_CART_BANK_SIZE + PB_CART_LINE_SIZE),
         "phone switch bank line was unexpectedly present");
  expect(capture.count == 1, "switch bank request count mismatch");
  expect(capture.bank == 1 && capture.offset == PB_CART_LINE_SIZE &&
         capture.size == PB_CART_LINE_SIZE, "switch bank did not request one cache line");
  expect(capture.demand, "switch bank request was not marked as demand");
  printf("cart cache phone fixed-bank fill test passed fixed=%u switch=%u\n",
         (unsigned)PB_CART_BANK_SIZE, (unsigned)PB_CART_LINE_SIZE);
}

int main(void) {
  test_active_bank_retention();
  test_phone_prefetch();
  test_phone_fixed_bank_fill_size();
  return 0;
}
