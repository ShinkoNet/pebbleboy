struct gb_s;
#include <stdbool.h>

#include "gb_audio.h"
#include "gb_cart.h"

void pb_core_rom_bank_changed(struct gb_s *gb);
bool pb_core_should_pause(struct gb_s *gb);
static __attribute__((noinline))
uint8_t prv_core_rom_read_miss(struct gb_s *gb, uint32_t rom_addr);
static __attribute__((noinline))
uint8_t prv_core_rom_read(struct gb_s *gb, uint_fast32_t addr);

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 1
#define PEANUT_FULL_GBC_SUPPORT 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_INTRINSICS 1
#define PGB_UNREACHABLE() __builtin_unreachable()
#define PEANUT_GB_ROM_BANK_CHANGED(gb) pb_core_rom_bank_changed(gb)
#define PEANUT_GB_SHOULD_PAUSE(gb) pb_core_should_pause(gb)
#define PEANUT_GB_ROM_READ(gb, addr) prv_core_rom_read((gb), (addr))
#include "peanut_gb.h"

static __attribute__((noinline))
uint8_t prv_core_rom_read(struct gb_s *gb, uint_fast32_t addr) {
  uint32_t rom_addr = (uint32_t)addr;
  uint32_t offset = rom_addr - gb->direct.rom_cache_start;
  if (offset < gb->direct.rom_cache_size) {
    return gb->direct.rom_cache_data[offset];
  }

  return prv_core_rom_read_miss(gb, rom_addr);
}

static __attribute__((noinline))
uint8_t prv_core_rom_read_miss(struct gb_s *gb, uint32_t rom_addr) {
  PbCart *cart = (PbCart *)gb->direct.priv;
  uint8_t value = pb_cart_read(cart, rom_addr);
  /* Resource and desktop-memory fills are synchronous. Phone fills can evict
   * slots asynchronously, so leave them on the validated PbCart path. */
  if (cart->mode != PB_CART_MODE_PHONE && cart->last_read_slot < PB_CART_CACHE_SLOTS) {
    PbCartSlot *slot = &cart->slots[cart->last_read_slot];
    uint32_t slot_offset = rom_addr - (uint32_t)slot->start;
    if (slot->valid && slot->start >= 0 && slot_offset < slot->size) {
      gb->direct.rom_cache_data = slot->data;
      gb->direct.rom_cache_start = (uint32_t)slot->start;
      gb->direct.rom_cache_size = slot->size;
    }
  }
  return value;
}
