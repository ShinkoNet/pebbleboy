#include "gb_core.h"

void __attribute__((weak)) pb_core_rom_bank_changed(struct gb_s *gb) {
  (void)gb;
}

bool __attribute__((weak)) pb_core_should_pause(struct gb_s *gb) {
  (void)gb;
  return false;
}
