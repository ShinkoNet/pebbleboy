#ifndef PB_GB_CART_H
#define PB_GB_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define PB_CART_BANK_SIZE 0x4000u
#define PB_CART_CACHE_BANKS 4
#define PB_CART_CACHE_BYTES (PB_CART_BANK_SIZE * PB_CART_CACHE_BANKS)
#define PB_CART_LINE_SIZE 0x1000u
#define PB_CART_FILL_SIZE 0x1000u
#define PB_CART_SLOT_SIZE PB_CART_LINE_SIZE
#define PB_CART_CACHE_SLOTS (PB_CART_CACHE_BYTES / PB_CART_LINE_SIZE)
#define PB_CART_BANK0_SLOTS (PB_CART_BANK_SIZE / PB_CART_LINE_SIZE)

typedef enum {
  PB_CART_MODE_NONE = 0,
  PB_CART_MODE_RESOURCE,
  PB_CART_MODE_PHONE,
  PB_CART_MODE_MEMORY,
} PbCartMode;

typedef bool (*PbCartBankRequestCb)(uint16_t bank, uint16_t offset, uint16_t size,
                                    void *context);

typedef struct {
  uint32_t hits;
  uint32_t misses;
  uint32_t loads;
  uint32_t phone_requests;
  uint32_t failed_loads;
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
  char error[64];
  PbCartBankRequestCb request_cb;
  void *request_context;
  PbCartSlot slots[PB_CART_CACHE_SLOTS];
  PbCartStats stats;
  uint32_t tick;
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
#endif

#ifdef PB_DESKTOP
bool pb_cart_init_memory(PbCart *cart, const uint8_t *rom, uint32_t rom_size);
#endif

bool pb_cart_init_phone(PbCart *cart, uint32_t rom_size, PbCartBankRequestCb request_cb,
                        void *request_context);

uint8_t pb_cart_read(PbCart *cart, uint32_t addr);
bool pb_cart_ensure_addr(PbCart *cart, uint32_t addr);
bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank);
bool pb_cart_ensure_fixed_bank(PbCart *cart);
bool pb_cart_has_bank(const PbCart *cart, uint16_t bank);
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
