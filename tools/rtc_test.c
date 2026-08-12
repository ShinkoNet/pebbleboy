#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_core.h"

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "RTC test failed: %s\n", message);
    exit(1);
  }
}

int main(void) {
  struct gb_s gb;
  memset(&gb, 0, sizeof(gb));
  gb.cart_has_rtc = true;
  gb.cart_rtc[0] = 59;
  gb.cart_rtc[1] = 59;
  gb.cart_rtc[2] = 23;
  gb.cart_rtc[3] = 0xFF;

  gb_advance_rtc(&gb, 1);
  expect(gb.cart_rtc[0] == 0 && gb.cart_rtc[1] == 0 && gb.cart_rtc[2] == 0,
         "day rollover time");
  expect(gb.cart_rtc[3] == 0 && gb.cart_rtc[4] == 1, "day 255 to 256");

  gb.cart_rtc[0] = 59;
  gb.cart_rtc[1] = 59;
  gb.cart_rtc[2] = 23;
  gb.cart_rtc[3] = 0xFF;
  gb.cart_rtc[4] = 1;
  gb_advance_rtc(&gb, 1);
  expect(gb.cart_rtc[3] == 0 && gb.cart_rtc[4] == 0x80,
         "512-day wrap and carry");

  memcpy(gb.cart_rtc, (uint8_t[]){1, 2, 3, 4, 0x40}, 5);
  gb_advance_rtc(&gb, 86400);
  expect(memcmp(gb.cart_rtc, (uint8_t[]){1, 2, 3, 4, 0x40}, 5) == 0,
         "halted clock advanced");

  puts("MBC3 RTC elapsed-time tests passed");
  return 0;
}
