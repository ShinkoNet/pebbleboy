#include "gb_cart.h"

#include <string.h>

#if defined(__GNUC__)
#pragma GCC optimize("O3")
#endif

#define PB_LOG(fmt, ...) APP_LOG(APP_LOG_LEVEL_DEBUG, "cart: " fmt, ##__VA_ARGS__)

static uint16_t prv_bank_count(uint32_t rom_size) {
  return (uint16_t)(rom_size / PB_CART_BANK_SIZE);
}

static uint32_t prv_line_start(uint32_t addr) {
  return addr & ~(PB_CART_LINE_SIZE - 1u);
}

static uint16_t prv_line_bank(uint32_t start) {
  return (uint16_t)(start / PB_CART_BANK_SIZE);
}

static uint16_t prv_hash_bucket(uint32_t start) {
  uint32_t line = start / PB_CART_LINE_SIZE;
  line ^= line >> 16;
  line *= 0x45D9F3Bu;
  line ^= line >> 16;
  return (uint16_t)(line % PB_CART_HASH_BUCKETS);
}

static void prv_index_remove(PbCart *cart, PbCartIndex slot_index) {
  PbCartSlot *slot = &cart->slots[slot_index];
  if (slot->start == PB_CART_START_NONE) {
    return;
  }

  uint16_t bucket = prv_hash_bucket(slot->start);
  PbCartIndex current = cart->hash_buckets[bucket];
  PbCartIndex previous = PB_CART_SLOT_NONE;
  while (current != PB_CART_SLOT_NONE) {
    if (current == slot_index) {
      if (previous == PB_CART_SLOT_NONE) {
        cart->hash_buckets[bucket] = slot->hash_next;
      } else {
        cart->slots[previous].hash_next = slot->hash_next;
      }
      break;
    }
    previous = current;
    current = cart->slots[current].hash_next;
  }
  slot->hash_next = PB_CART_SLOT_NONE;
}

static void prv_index_add(PbCart *cart, PbCartIndex slot_index) {
  PbCartSlot *slot = &cart->slots[slot_index];
  uint16_t bucket = prv_hash_bucket(slot->start);
  slot->hash_next = cart->hash_buckets[bucket];
  cart->hash_buckets[bucket] = slot_index;
}

static void prv_lru_remove(PbCart *cart, PbCartIndex slot_index) {
  PbCartSlot *slot = &cart->slots[slot_index];
  bool linked = cart->lru_head == slot_index || slot->lru_prev != PB_CART_SLOT_NONE ||
                slot->lru_next != PB_CART_SLOT_NONE;
  if (!linked) {
    return;
  }
  if (slot->lru_prev != PB_CART_SLOT_NONE) {
    cart->slots[slot->lru_prev].lru_next = slot->lru_next;
  } else {
    cart->lru_head = slot->lru_next;
  }
  if (slot->lru_next != PB_CART_SLOT_NONE) {
    cart->slots[slot->lru_next].lru_prev = slot->lru_prev;
  } else {
    cart->lru_tail = slot->lru_prev;
  }
  slot->lru_prev = PB_CART_SLOT_NONE;
  slot->lru_next = PB_CART_SLOT_NONE;
}

static void prv_lru_touch(PbCart *cart, PbCartIndex slot_index) {
  PbCartSlot *slot = &cart->slots[slot_index];
  prv_lru_remove(cart, slot_index);
  slot->lru_prev = PB_CART_SLOT_NONE;
  slot->lru_next = cart->lru_head;
  if (cart->lru_head != PB_CART_SLOT_NONE) {
    cart->slots[cart->lru_head].lru_prev = slot_index;
  } else {
    cart->lru_tail = slot_index;
  }
  cart->lru_head = slot_index;
}

static PbCartIndex prv_find_slot_index(const PbCart *cart, uint32_t start) {
  PbCartIndex slot = cart->hash_buckets[prv_hash_bucket(start)];
  while (slot != PB_CART_SLOT_NONE) {
    if (cart->slots[slot].start == start) {
      return slot;
    }
    slot = cart->slots[slot].hash_next;
  }
  return PB_CART_SLOT_NONE;
}

static PbCartIndex prv_select_slot(PbCart *cart, uint32_t start) {
  PbCartIndex existing = prv_find_slot_index(cart, start);
  if (existing != PB_CART_SLOT_NONE) {
    return existing;
  }
  for (unsigned i = 0; i < PB_CART_CACHE_SLOTS; i++) {
    if (cart->slots[i].start == PB_CART_START_NONE) {
      return (PbCartIndex)i;
    }
  }
  return cart->lru_tail != PB_CART_SLOT_NONE ? cart->lru_tail : 0;
}

static bool prv_load_line(PbCart *cart, uint32_t start) {
  if (start >= cart->rom_size) {
    pb_cart_set_error(cart, "read outside ROM");
    return false;
  }

  PbCartIndex slot_index = prv_select_slot(cart, start);
  PbCartSlot *slot = &cart->slots[slot_index];
  if (cart->last_read_slot == slot_index) {
    cart->last_read_slot = PB_CART_SLOT_NONE;
  }
  prv_index_remove(cart, slot_index);
  prv_lru_remove(cart, slot_index);
  slot->start = PB_CART_START_NONE;

  int loaded = -1;
  if (cart->mode == PB_CART_MODE_RESOURCE) {
    loaded = (int)resource_load_byte_range(cart->resource, start, slot->data,
                                           PB_CART_LINE_SIZE);
#ifdef PEBBLEBOY_APP_BLOB
  } else if (cart->mode == PB_CART_MODE_BLOB) {
    loaded = app_blob_read(start, slot->data, PB_CART_LINE_SIZE);
#endif
  }
  if (loaded != (int)PB_CART_LINE_SIZE) {
    cart->stats.failed_loads++;
    pb_cart_set_error(cart, cart->mode == PB_CART_MODE_BLOB
                               ? "flash ROM read failed"
                               : "resource ROM read failed");
    return false;
  }

  slot->start = start;
  prv_index_add(cart, slot_index);
  prv_lru_touch(cart, slot_index);
  cart->last_read_slot = slot_index;
  cart->stats.loads++;
  cart->stats.source_reads++;
  cart->stats.source_bytes += PB_CART_LINE_SIZE;
  cart->stats.last_load_bank = prv_line_bank(start);
  if (cart->stats.loads <= 2) {
    PB_LOG("bank %u line %u loaded", (unsigned)prv_line_bank(start),
           (unsigned)(start & (PB_CART_BANK_SIZE - 1u)));
  }
  return true;
}

void pb_cart_init_empty(PbCart *cart) {
  memset(cart, 0, sizeof(*cart));
  cart->mode = PB_CART_MODE_NONE;
  cart->lru_head = PB_CART_SLOT_NONE;
  cart->lru_tail = PB_CART_SLOT_NONE;
  cart->last_read_slot = PB_CART_SLOT_NONE;
  for (unsigned i = 0; i < PB_CART_CACHE_SLOTS; i++) {
    cart->slots[i].start = PB_CART_START_NONE;
    cart->slots[i].hash_next = PB_CART_SLOT_NONE;
    cart->slots[i].lru_prev = PB_CART_SLOT_NONE;
    cart->slots[i].lru_next = PB_CART_SLOT_NONE;
  }
  memset(cart->hash_buckets, PB_CART_SLOT_NONE, sizeof(cart->hash_buckets));
}

static bool prv_validate_rom(PbCart *cart) {
  if (cart->rom_size < 2u * PB_CART_BANK_SIZE ||
      cart->rom_size % PB_CART_BANK_SIZE != 0) {
    pb_cart_set_error(cart, "invalid ROM size");
    return false;
  }
  cart->bank_count = prv_bank_count(cart->rom_size);
  return prv_load_line(cart, 0);
}

bool pb_cart_init_resource(PbCart *cart, uint32_t resource_id) {
  pb_cart_init_empty(cart);
  cart->mode = PB_CART_MODE_RESOURCE;
  cart->resource_id = resource_id;
  cart->resource = resource_get_handle(resource_id);
  cart->rom_size = (uint32_t)resource_size(cart->resource);
  return prv_validate_rom(cart);
}

#ifdef PEBBLEBOY_APP_BLOB
bool pb_cart_init_blob(PbCart *cart, uint32_t rom_size) {
  pb_cart_init_empty(cart);
  cart->mode = PB_CART_MODE_BLOB;
  cart->rom_size = rom_size;
  return prv_validate_rom(cart);
}
#endif

bool pb_cart_ensure_addr(PbCart *cart, uint32_t addr) {
  if (addr >= cart->rom_size) {
    pb_cart_set_error(cart, "address outside ROM");
    return false;
  }
  uint32_t start = prv_line_start(addr);
  if (prv_find_slot_index(cart, start) != PB_CART_SLOT_NONE) {
    return true;
  }
  cart->stats.misses++;
  cart->stats.last_miss_bank = prv_line_bank(start);
  return prv_load_line(cart, start);
}

bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank) {
  return pb_cart_ensure_addr(cart, (uint32_t)bank * PB_CART_BANK_SIZE);
}

uint8_t pb_cart_read_slow(PbCart *cart, uint32_t addr) {
  if (addr >= cart->rom_size) {
    return 0xFF;
  }
  uint32_t start = prv_line_start(addr);
  PbCartIndex slot_index = prv_find_slot_index(cart, start);
  if (slot_index == PB_CART_SLOT_NONE) {
    cart->stats.misses++;
    cart->stats.last_miss_bank = prv_line_bank(start);
    if (!prv_load_line(cart, start)) {
      return 0xFF;
    }
    slot_index = cart->last_read_slot;
  }
  if (slot_index == PB_CART_SLOT_NONE) {
    return 0xFF;
  }
  if (slot_index != cart->last_read_slot) {
    prv_lru_touch(cart, slot_index);
    cart->last_read_slot = slot_index;
  }
  return cart->slots[slot_index].data[addr - start];
}

bool pb_cart_has_bank(const PbCart *cart, uint16_t bank) {
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE;
  return start < cart->rom_size &&
         prv_find_slot_index(cart, start) != PB_CART_SLOT_NONE;
}

void pb_cart_set_error(PbCart *cart, const char *message) {
  cart->failed = true;
  strncpy(cart->error, message ? message : "cart error", sizeof(cart->error) - 1);
  cart->error[sizeof(cart->error) - 1] = '\0';
  PB_LOG("%s", cart->error);
}

const PbCartStats *pb_cart_stats(const PbCart *cart) {
  return &cart->stats;
}
