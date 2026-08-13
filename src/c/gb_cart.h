#ifndef PB_GB_CART_H
#define PB_GB_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gb_blob.h"

#define PB_CART_BANK_SIZE 0x4000u
#define PB_CART_LINE_SIZE 0x0080u
/* The 192-line working set was selected from physical Crystal profiling. All
 * builds use it; firmware/API availability must not change emulator speed. */
#define PB_CART_CACHE_SLOTS 192u
#define PB_CART_CACHE_BYTES (PB_CART_CACHE_SLOTS * PB_CART_LINE_SIZE)
#define PB_CART_HASH_BUCKETS (PB_CART_CACHE_SLOTS * 2u)

typedef uint8_t PbCartIndex;
#define PB_CART_SLOT_NONE UINT8_MAX
#define PB_CART_START_NONE UINT32_MAX

typedef enum {
  PB_CART_MODE_NONE = 0,
  PB_CART_MODE_RESOURCE,
  PB_CART_MODE_BLOB,
} PbCartMode;

typedef struct {
  uint32_t misses;
  uint32_t loads;
  uint32_t failed_loads;
  uint32_t source_reads;
  uint32_t source_bytes;
  uint16_t last_miss_bank;
  uint16_t last_load_bank;
} PbCartStats;

/* ROM sizes are complete 16 KiB banks, so every 128-byte line is full. Slot
 * validity is encoded by start rather than separate valid/loading flags. The
 * cache has fewer than 255 entries, allowing one-byte hash and LRU links. */
typedef struct {
  uint32_t start;
  uint8_t data[PB_CART_LINE_SIZE];
  PbCartIndex hash_next;
  PbCartIndex lru_prev;
  PbCartIndex lru_next;
} PbCartSlot;

typedef struct {
  PbCartMode mode;
  uint32_t rom_size;
  uint16_t bank_count;
  bool failed;
  char error[64];
  PbCartSlot slots[PB_CART_CACHE_SLOTS];
  PbCartIndex hash_buckets[PB_CART_HASH_BUCKETS];
  PbCartStats stats;
  PbCartIndex lru_head;
  PbCartIndex lru_tail;
  PbCartIndex last_read_slot;
  ResHandle resource;
  uint32_t resource_id;
} PbCart;

void pb_cart_init_empty(PbCart *cart);
bool pb_cart_init_resource(PbCart *cart, uint32_t resource_id);
#ifdef PEBBLEBOY_APP_BLOB
bool pb_cart_init_blob(PbCart *cart, uint32_t rom_size);
#endif

uint8_t pb_cart_read_slow(PbCart *cart, uint32_t addr);

/* Instruction fetches normally stay within one line for hundreds of reads. */
static inline __attribute__((always_inline))
uint8_t pb_cart_read(PbCart *cart, uint32_t addr) {
  PbCartIndex slot_index = cart->last_read_slot;
  if (slot_index < PB_CART_CACHE_SLOTS && addr < cart->rom_size) {
    PbCartSlot *slot = &cart->slots[slot_index];
    uint32_t offset = addr - slot->start;
    if (slot->start != PB_CART_START_NONE && offset < PB_CART_LINE_SIZE) {
      return slot->data[offset];
    }
  }
  return pb_cart_read_slow(cart, addr);
}

bool pb_cart_ensure_addr(PbCart *cart, uint32_t addr);
bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank);
bool pb_cart_has_bank(const PbCart *cart, uint16_t bank);
void pb_cart_set_error(PbCart *cart, const char *message);
const PbCartStats *pb_cart_stats(const PbCart *cart);

#endif
