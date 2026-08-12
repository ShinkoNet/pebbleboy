#ifndef PB_GB_CART_H
#define PB_GB_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PB_DESKTOP
#include "gb_blob.h"
#endif

#define PB_CART_BANK_SIZE 0x4000u
#ifndef PB_CART_LINE_SIZE
/* Local flash has low transfer overhead, and a finer-grained cache avoids
 * retaining cold bytes from the fixed and switched ROM regions. */
#define PB_CART_LINE_SIZE 0x0080u
#endif
#define PB_CART_FILL_SIZE PB_CART_BANK_SIZE
#define PB_CART_SLOT_SIZE PB_CART_LINE_SIZE
#define PB_CART_BANK0_SLOTS (PB_CART_BANK_SIZE / PB_CART_LINE_SIZE)
#ifdef PB_CART_CACHE_BANKS
/* Retain full-bank cache configurations for the desktop profiler and the
 * legacy phone-streaming tests. */
#define PB_CART_CACHE_BYTES (PB_CART_BANK_SIZE * PB_CART_CACHE_BANKS)
#define PB_CART_CACHE_SLOTS (PB_CART_CACHE_BYTES / PB_CART_LINE_SIZE)
#define PB_CART_PINNED_SLOTS PB_CART_BANK0_SLOTS
#else
/* Resource and app-blob reads are synchronous. Fixed and switched ROM regions
 * can therefore share one small LRU instead of permanently pinning the whole
 * 16 KiB fixed bank. Fifty-six lines keep Crystal's traced flash traffic low
 * while leaving several kilobytes of safety margin in a stock 128 KiB app. */
#ifndef PB_CART_CACHE_SLOTS
#define PB_CART_CACHE_SLOTS 56u
#endif
#define PB_CART_CACHE_BYTES (PB_CART_CACHE_SLOTS * PB_CART_LINE_SIZE)
#define PB_CART_PINNED_SLOTS 0u
#endif
#define PB_CART_ACTIVE_BANK_NONE UINT16_MAX
#define PB_CART_LOOKUP_HINTS 4
#ifndef PB_CART_TRACK_HITS
#ifdef PB_DESKTOP
#define PB_CART_TRACK_HITS 1
#else
#define PB_CART_TRACK_HITS 0
#endif
#endif

typedef enum {
  PB_CART_MODE_NONE = 0,
  PB_CART_MODE_RESOURCE,
  PB_CART_MODE_BLOB,
  PB_CART_MODE_PHONE,
  PB_CART_MODE_MEMORY,
} PbCartMode;

typedef bool (*PbCartBankRequestCb)(uint16_t bank, uint16_t offset, uint16_t size,
                                    bool demand, void *context);

typedef struct {
  uint32_t hits;
  uint32_t misses;
  uint32_t loads;
  uint32_t phone_requests;
  uint32_t failed_loads;
  uint32_t source_reads;
  uint32_t source_bytes;
  uint16_t last_miss_bank;
  uint16_t last_load_bank;
  uint64_t load_bank_mask;
  uint64_t request_bank_mask;
  uint32_t last_load_ms;
} PbCartStats;

typedef struct {
  int32_t start;
  bool valid;
  bool loading;
  uint16_t size;
  uint16_t received;
  uint32_t last_used;
  uint8_t data[PB_CART_SLOT_SIZE];
} PbCartSlot;

typedef struct {
  PbCartMode mode;
  uint32_t rom_size;
  uint16_t bank_count;
  bool paused;
  bool failed;
  bool read_faulted;
  uint32_t pending_start;
  uint32_t read_fault_start;
  uint16_t active_bank;
  char error[64];
  PbCartBankRequestCb request_cb;
  void *request_context;
  PbCartSlot slots[PB_CART_CACHE_SLOTS];
  uint16_t lookup_hints[PB_CART_LOOKUP_HINTS];
  PbCartStats stats;
  uint32_t tick;
  uint16_t last_read_slot;
#ifndef PB_DESKTOP
  ResHandle resource;
  uint32_t resource_id;
#else
  const uint8_t *memory;
#endif
} PbCart;

void pb_cart_init_empty(PbCart *cart);

#ifndef PB_DESKTOP
bool pb_cart_init_resource(PbCart *cart, uint32_t resource_id);
#ifdef PEBBLEBOY_APP_BLOB
bool pb_cart_init_blob(PbCart *cart, uint32_t rom_size);
#endif
#endif

#ifdef PB_DESKTOP
bool pb_cart_init_memory(PbCart *cart, const uint8_t *rom, uint32_t rom_size);
#endif

bool pb_cart_init_phone(PbCart *cart, uint32_t rom_size, PbCartBankRequestCb request_cb,
                        void *request_context);

uint8_t pb_cart_read_slow(PbCart *cart, uint32_t addr);

/* CPU instruction fetches normally stay within one 512-byte cache line for
 * hundreds of reads. Keep that overwhelmingly common path inline so the core
 * avoids another function call and a cache search for every ROM byte. */
static inline uint8_t pb_cart_read(PbCart *cart, uint32_t addr) {
  uint16_t slot_index = cart->last_read_slot;
  if (slot_index < PB_CART_CACHE_SLOTS && addr < cart->rom_size) {
    PbCartSlot *slot = &cart->slots[slot_index];
    if (slot->valid && slot->start >= 0) {
      uint32_t offset = addr - (uint32_t)slot->start;
      if (offset < slot->size) {
#if PB_CART_TRACK_HITS
        cart->stats.hits++;
#endif
        return slot->data[offset];
      }
    }
  }
  return pb_cart_read_slow(cart, addr);
}
bool pb_cart_ensure_addr(PbCart *cart, uint32_t addr);
bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank);
bool pb_cart_ensure_fixed_bank(PbCart *cart);
bool pb_cart_has_bank(const PbCart *cart, uint16_t bank);
bool pb_cart_prefetch_addr(PbCart *cart, uint32_t addr);
void pb_cart_set_active_bank(PbCart *cart, uint16_t bank);
bool pb_cart_paused(const PbCart *cart);
bool pb_cart_read_faulted(const PbCart *cart);
void pb_cart_clear_read_fault(PbCart *cart);
void pb_cart_resume(PbCart *cart);
void pb_cart_set_error(PbCart *cart, const char *message);
const PbCartStats *pb_cart_stats(const PbCart *cart);

bool pb_cart_phone_begin(PbCart *cart, uint16_t bank, uint16_t offset, uint16_t size);
bool pb_cart_phone_data(PbCart *cart, uint16_t bank, uint16_t offset, const uint8_t *data,
                        uint16_t len);
bool pb_cart_phone_end(PbCart *cart, uint16_t bank, uint16_t offset, uint16_t size);

#endif
