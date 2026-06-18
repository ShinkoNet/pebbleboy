#include "gb_cart.h"

#include <string.h>

#ifdef PB_DESKTOP
#include <stdio.h>
#define PB_LOG(fmt, ...) fprintf(stderr, "cart: " fmt "\n", ##__VA_ARGS__)
#else
#define PB_LOG(fmt, ...) APP_LOG(APP_LOG_LEVEL_INFO, "cart: " fmt, ##__VA_ARGS__)
#endif

static uint16_t prv_bank_count(uint32_t rom_size) {
  return (uint16_t)((rom_size + PB_CART_BANK_SIZE - 1) / PB_CART_BANK_SIZE);
}

static uint16_t prv_bank_size(const PbCart *cart, uint16_t bank) {
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE;
  if (start >= cart->rom_size) {
    return 0;
  }
  uint32_t left = cart->rom_size - start;
  return (uint16_t)(left > PB_CART_BANK_SIZE ? PB_CART_BANK_SIZE : left);
}

static PbCartSlot *prv_find_slot(PbCart *cart, uint16_t bank) {
  for (int i = 0; i < PB_CART_CACHE_BANKS; i++) {
    if (cart->slots[i].bank == bank) {
      return &cart->slots[i];
    }
  }
  return NULL;
}

static const PbCartSlot *prv_find_const_slot(const PbCart *cart, uint16_t bank) {
  for (int i = 0; i < PB_CART_CACHE_BANKS; i++) {
    if (cart->slots[i].bank == bank) {
      return &cart->slots[i];
    }
  }
  return NULL;
}

static PbCartSlot *prv_select_slot(PbCart *cart, uint16_t bank) {
  PbCartSlot *existing = prv_find_slot(cart, bank);
  if (existing) {
    return existing;
  }

  if (bank == 0) {
    return &cart->slots[0];
  }

  for (int i = 1; i < PB_CART_CACHE_BANKS; i++) {
    if (!cart->slots[i].valid && !cart->slots[i].loading) {
      return &cart->slots[i];
    }
  }

  int victim = 1;
  for (int i = 2; i < PB_CART_CACHE_BANKS; i++) {
    if (cart->slots[i].last_used < cart->slots[victim].last_used) {
      victim = i;
    }
  }
  return &cart->slots[victim];
}

static void prv_prepare_slot(PbCartSlot *slot, uint16_t bank) {
  slot->bank = bank;
  slot->valid = false;
  slot->loading = false;
  slot->received = 0;
  memset(slot->data, 0xFF, PB_CART_BANK_SIZE);
}

void pb_cart_init_empty(PbCart *cart) {
  memset(cart, 0, sizeof(*cart));
  cart->mode = PB_CART_MODE_NONE;
  cart->pending_bank = UINT16_MAX;
  for (int i = 0; i < PB_CART_CACHE_BANKS; i++) {
    cart->slots[i].bank = -1;
  }
}

static bool prv_load_bank(PbCart *cart, uint16_t bank) {
  if (bank >= cart->bank_count) {
    pb_cart_set_error(cart, "bank outside ROM");
    return false;
  }

  PbCartSlot *slot = prv_select_slot(cart, bank);
  prv_prepare_slot(slot, bank);
  uint16_t size = prv_bank_size(cart, bank);

#ifdef PB_DESKTOP
  if (cart->mode == PB_CART_MODE_MEMORY) {
    memcpy(slot->data, cart->memory + (uint32_t)bank * PB_CART_BANK_SIZE, size);
  } else
#endif
#ifndef PB_DESKTOP
  if (cart->mode == PB_CART_MODE_RESOURCE) {
    size_t loaded = resource_load_byte_range(cart->resource,
                                             (uint32_t)bank * PB_CART_BANK_SIZE,
                                             slot->data, size);
    if (loaded != size) {
      cart->stats.failed_loads++;
      pb_cart_set_error(cart, "resource read failed");
      return false;
    }
  } else
#endif
  {
    pb_cart_set_error(cart, "unsupported ROM source");
    return false;
  }

  slot->valid = true;
  slot->loading = false;
  slot->received = size;
  slot->last_used = ++cart->tick;
  cart->stats.loads++;
  return true;
}

#ifndef PB_DESKTOP
bool pb_cart_init_resource(PbCart *cart, uint32_t resource_id) {
  pb_cart_init_empty(cart);
  cart->mode = PB_CART_MODE_RESOURCE;
  cart->resource_id = resource_id;
  cart->resource = resource_get_handle(resource_id);
  cart->rom_size = (uint32_t)resource_size(cart->resource);
  cart->bank_count = prv_bank_count(cart->rom_size);
  if (cart->rom_size < 0x150 || cart->bank_count == 0) {
    pb_cart_set_error(cart, "resource ROM missing");
    return false;
  }
  return prv_load_bank(cart, 0);
}
#endif

#ifdef PB_DESKTOP
bool pb_cart_init_memory(PbCart *cart, const uint8_t *rom, uint32_t rom_size) {
  pb_cart_init_empty(cart);
  cart->mode = PB_CART_MODE_MEMORY;
  cart->memory = rom;
  cart->rom_size = rom_size;
  cart->bank_count = prv_bank_count(rom_size);
  if (!rom || rom_size < 0x150 || cart->bank_count == 0) {
    pb_cart_set_error(cart, "memory ROM missing");
    return false;
  }
  return prv_load_bank(cart, 0);
}
#endif

bool pb_cart_init_phone(PbCart *cart, uint32_t rom_size, PbCartBankRequestCb request_cb,
                        void *request_context) {
  pb_cart_init_empty(cart);
  cart->mode = PB_CART_MODE_PHONE;
  cart->rom_size = rom_size;
  cart->bank_count = prv_bank_count(rom_size);
  cart->request_cb = request_cb;
  cart->request_context = request_context;
  if (rom_size < 0x150 || cart->bank_count == 0) {
    pb_cart_set_error(cart, "phone ROM missing");
    return false;
  }
  return true;
}

static void prv_request_phone_bank(PbCart *cart, uint16_t bank) {
  PbCartSlot *slot = prv_select_slot(cart, bank);
  if (slot->loading && slot->bank == bank) {
    cart->paused = true;
    cart->pending_bank = bank;
    return;
  }
  prv_prepare_slot(slot, bank);
  slot->loading = true;
  cart->paused = true;
  cart->pending_bank = bank;
  cart->stats.phone_requests++;
  if (cart->request_cb) {
    cart->request_cb(bank, cart->request_context);
  }
}

bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank) {
  if (pb_cart_has_bank(cart, bank)) {
    return true;
  }
  cart->stats.misses++;
  cart->stats.last_miss_bank = bank;
  if (cart->mode == PB_CART_MODE_PHONE) {
    prv_request_phone_bank(cart, bank);
    return false;
  }
  return prv_load_bank(cart, bank);
}

uint8_t pb_cart_read(PbCart *cart, uint32_t addr) {
  if (addr >= cart->rom_size) {
    return 0xFF;
  }

  uint16_t bank = (uint16_t)(addr / PB_CART_BANK_SIZE);
  uint16_t offset = (uint16_t)(addr & (PB_CART_BANK_SIZE - 1));
  PbCartSlot *slot = prv_find_slot(cart, bank);
  if (!slot || !slot->valid) {
    if (!pb_cart_ensure_bank(cart, bank)) {
      return 0xFF;
    }
    slot = prv_find_slot(cart, bank);
  }

  if (!slot || !slot->valid) {
    return 0xFF;
  }
  slot->last_used = ++cart->tick;
  cart->stats.hits++;
  return slot->data[offset];
}

bool pb_cart_has_bank(const PbCart *cart, uint16_t bank) {
  const PbCartSlot *slot = prv_find_const_slot(cart, bank);
  return slot && slot->valid;
}

bool pb_cart_paused(const PbCart *cart) {
  return cart->paused || cart->failed;
}

void pb_cart_resume(PbCart *cart) {
  if (!cart->failed) {
    cart->paused = false;
    cart->pending_bank = UINT16_MAX;
  }
}

void pb_cart_set_error(PbCart *cart, const char *message) {
  cart->failed = true;
  cart->paused = true;
  strncpy(cart->error, message ? message : "cart error", sizeof(cart->error) - 1);
  cart->error[sizeof(cart->error) - 1] = '\0';
  PB_LOG("%s", cart->error);
}

const PbCartStats *pb_cart_stats(const PbCart *cart) {
  return &cart->stats;
}

bool pb_cart_phone_begin(PbCart *cart, uint16_t bank, uint16_t size) {
  if (cart->mode != PB_CART_MODE_PHONE || bank >= cart->bank_count || size > PB_CART_BANK_SIZE) {
    return false;
  }
  PbCartSlot *slot = prv_select_slot(cart, bank);
  prv_prepare_slot(slot, bank);
  slot->loading = true;
  slot->last_used = ++cart->tick;
  return true;
}

bool pb_cart_phone_data(PbCart *cart, uint16_t bank, uint16_t offset, const uint8_t *data,
                        uint16_t len) {
  PbCartSlot *slot = prv_find_slot(cart, bank);
  if (!slot || !slot->loading || offset + len > PB_CART_BANK_SIZE) {
    return false;
  }
  memcpy(slot->data + offset, data, len);
  uint16_t end = offset + len;
  if (end > slot->received) {
    slot->received = end;
  }
  slot->last_used = ++cart->tick;
  return true;
}

bool pb_cart_phone_end(PbCart *cart, uint16_t bank, uint16_t size) {
  PbCartSlot *slot = prv_find_slot(cart, bank);
  if (!slot || !slot->loading) {
    return false;
  }
  uint16_t expected = size ? size : prv_bank_size(cart, bank);
  if (slot->received < expected) {
    cart->stats.failed_loads++;
    pb_cart_set_error(cart, "incomplete phone bank");
    return false;
  }
  slot->valid = true;
  slot->loading = false;
  slot->last_used = ++cart->tick;
  cart->stats.loads++;
  if (cart->pending_bank == bank) {
    pb_cart_resume(cart);
  }
  return true;
}

