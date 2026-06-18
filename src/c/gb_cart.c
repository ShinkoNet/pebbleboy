#include "gb_cart.h"

#include <string.h>

#ifdef PB_DESKTOP
#define PB_LOG(fmt, ...)
#else
#define PB_LOG(fmt, ...) APP_LOG(APP_LOG_LEVEL_INFO, "cart: " fmt, ##__VA_ARGS__)
#endif

#ifndef PB_DESKTOP
static const char *prv_mode_name(PbCartMode mode) {
  switch (mode) {
    case PB_CART_MODE_RESOURCE:
      return "resource";
    case PB_CART_MODE_PHONE:
      return "phone";
    case PB_CART_MODE_MEMORY:
      return "memory";
    case PB_CART_MODE_NONE:
    default:
      return "none";
  }
}
#endif

static void prv_note_bank(uint64_t *mask, uint16_t bank) {
  if (bank < 64) {
    *mask |= ((uint64_t)1) << bank;
  }
}

static uint16_t prv_bank_count(uint32_t rom_size) {
  return (uint16_t)((rom_size + PB_CART_BANK_SIZE - 1) / PB_CART_BANK_SIZE);
}

static uint32_t prv_slot_start(uint32_t addr) {
  return addr & ~(PB_CART_LINE_SIZE - 1u);
}

static uint32_t prv_fill_unit(const PbCart *cart, uint32_t addr) {
  return cart->bank_count <= 2 || addr < PB_CART_BANK_SIZE
      ? PB_CART_BANK_SIZE
      : PB_CART_LINE_SIZE;
}

static uint32_t prv_fill_start(const PbCart *cart, uint32_t addr) {
  uint32_t fill_unit = prv_fill_unit(cart, addr);
  return addr & ~(fill_unit - 1u);
}

static uint16_t prv_slot_size(const PbCart *cart, uint32_t start) {
  if (start >= cart->rom_size) {
    return 0;
  }
  uint32_t left = cart->rom_size - start;
  return (uint16_t)(left > PB_CART_LINE_SIZE ? PB_CART_LINE_SIZE : left);
}

static uint16_t prv_fill_size(const PbCart *cart, uint32_t start) {
  if (start >= cart->rom_size) {
    return 0;
  }
  uint32_t fill_unit = prv_fill_unit(cart, start);
  uint32_t bank_left = PB_CART_BANK_SIZE - (start & (PB_CART_BANK_SIZE - 1u));
  uint32_t rom_left = cart->rom_size - start;
  uint32_t left = bank_left < rom_left ? bank_left : rom_left;
  return (uint16_t)(left > fill_unit ? fill_unit : left);
}

static uint16_t prv_slot_bank(uint32_t start) {
  return (uint16_t)(start / PB_CART_BANK_SIZE);
}

static uint16_t prv_slot_bank_offset(uint32_t start) {
  return (uint16_t)(start & (PB_CART_BANK_SIZE - 1u));
}

static PbCartSlot *prv_find_slot(PbCart *cart, uint32_t start) {
  for (int i = 0; i < (int)PB_CART_CACHE_SLOTS; i++) {
    if (cart->slots[i].start == (int32_t)start) {
      return &cart->slots[i];
    }
  }
  return NULL;
}

static const PbCartSlot *prv_find_const_slot(const PbCart *cart, uint32_t start) {
  for (int i = 0; i < (int)PB_CART_CACHE_SLOTS; i++) {
    if (cart->slots[i].start == (int32_t)start) {
      return &cart->slots[i];
    }
  }
  return NULL;
}

static bool prv_has_loading_slots(const PbCart *cart) {
  for (int i = 0; i < (int)PB_CART_CACHE_SLOTS; i++) {
    if (cart->slots[i].loading) {
      return true;
    }
  }
  return false;
}

static bool prv_slot_is_active_bank(const PbCart *cart, const PbCartSlot *slot) {
  return cart->active_bank != PB_CART_ACTIVE_BANK_NONE && slot->start >= 0 &&
         prv_slot_bank((uint32_t)slot->start) == cart->active_bank;
}

static PbCartSlot *prv_select_slot(PbCart *cart, uint32_t start) {
  PbCartSlot *existing = prv_find_slot(cart, start);
  if (existing) {
    return existing;
  }

  if (prv_slot_bank(start) == 0) {
    uint16_t line = (uint16_t)(start / PB_CART_LINE_SIZE);
    if (line < PB_CART_BANK0_SLOTS) {
      return &cart->slots[line];
    }
  }

  for (int i = (int)PB_CART_BANK0_SLOTS; i < (int)PB_CART_CACHE_SLOTS; i++) {
    if (!cart->slots[i].valid && !cart->slots[i].loading) {
      return &cart->slots[i];
    }
  }

  int victim = -1;
  int active_victim = -1;
  for (int i = (int)PB_CART_BANK0_SLOTS; i < (int)PB_CART_CACHE_SLOTS; i++) {
    if (cart->slots[i].loading) {
      continue;
    }
    if (prv_slot_is_active_bank(cart, &cart->slots[i])) {
      if (active_victim < 0 ||
          cart->slots[i].last_used < cart->slots[active_victim].last_used) {
        active_victim = i;
      }
      continue;
    }
    if (victim < 0 || cart->slots[i].last_used < cart->slots[victim].last_used) {
      victim = i;
    }
  }
  if (victim < 0) {
    victim = active_victim;
  }
  if (victim < 0) {
    victim = (int)PB_CART_BANK0_SLOTS;
  }
  return &cart->slots[victim];
}

static void prv_prepare_slot(PbCartSlot *slot, uint32_t start, uint16_t size) {
  slot->start = (int32_t)start;
  slot->valid = false;
  slot->loading = false;
  slot->size = size;
  slot->received = 0;
  memset(slot->data, 0xFF, PB_CART_SLOT_SIZE);
}

void pb_cart_init_empty(PbCart *cart) {
  memset(cart, 0, sizeof(*cart));
  cart->mode = PB_CART_MODE_NONE;
  cart->pending_start = UINT32_MAX;
  cart->read_fault_start = UINT32_MAX;
  cart->active_bank = PB_CART_ACTIVE_BANK_NONE;
  for (int i = 0; i < (int)PB_CART_CACHE_SLOTS; i++) {
    cart->slots[i].start = -1;
  }
}

static bool prv_load_line_from_source(PbCart *cart, PbCartSlot *slot, uint32_t start) {
  uint16_t size = prv_slot_size(cart, start);
  prv_prepare_slot(slot, start, size);

#ifdef PB_DESKTOP
  if (cart->mode == PB_CART_MODE_MEMORY) {
    memcpy(slot->data, cart->memory + start, size);
  } else
#endif
#ifndef PB_DESKTOP
  if (cart->mode == PB_CART_MODE_RESOURCE) {
    size_t loaded = resource_load_byte_range(cart->resource, start, slot->data, size);
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
  return true;
}

static bool prv_load_fill(PbCart *cart, uint32_t requested_start) {
  uint32_t start = prv_fill_start(cart, requested_start);
  if (start >= cart->rom_size) {
    pb_cart_set_error(cart, "fill outside ROM");
    return false;
  }

  uint16_t fill_size = prv_fill_size(cart, start);
  uint32_t end = start + fill_size;
  for (uint32_t line_start = start; line_start < end; line_start += PB_CART_LINE_SIZE) {
    PbCartSlot *slot = prv_select_slot(cart, line_start);
    if (!prv_load_line_from_source(cart, slot, line_start)) {
      return false;
    }
  }

  cart->stats.loads++;
  uint16_t bank = prv_slot_bank(start);
  uint16_t offset = prv_slot_bank_offset(start);
  (void)offset;
  cart->stats.last_load_bank = bank;
  prv_note_bank(&cart->stats.load_bank_mask, bank);
  PB_LOG("%s bank %u fill %u loaded size=%u", prv_mode_name(cart->mode),
         (unsigned)bank, (unsigned)offset, (unsigned)fill_size);
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
  return prv_load_fill(cart, 0);
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
  return prv_load_fill(cart, 0);
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

static void prv_prepare_loading_fill(PbCart *cart, uint32_t start, uint16_t fill_size) {
  start = prv_slot_start(start);
  uint32_t end = start + fill_size;
  for (uint32_t line_start = start; line_start < end; line_start += PB_CART_LINE_SIZE) {
    PbCartSlot *slot = prv_select_slot(cart, line_start);
    prv_prepare_slot(slot, line_start, prv_slot_size(cart, line_start));
    slot->loading = true;
    slot->last_used = ++cart->tick;
  }
}

static bool prv_request_phone_fill(PbCart *cart, uint32_t requested_start, bool demand) {
  uint32_t line_start = prv_slot_start(requested_start);
  uint32_t start = prv_fill_start(cart, line_start);
  uint16_t bank = prv_slot_bank(start);
  uint16_t offset = prv_slot_bank_offset(start);
  uint16_t size = prv_fill_size(cart, start);
  PbCartSlot *slot = prv_find_slot(cart, line_start);
  if (slot && slot->loading) {
    if (demand) {
      cart->paused = true;
      cart->pending_start = line_start;
      return true;
    }
    return false;
  }

  if (prv_has_loading_slots(cart) || !cart->request_cb ||
      !cart->request_cb(bank, offset, size, demand, cart->request_context)) {
    return false;
  }

  prv_prepare_loading_fill(cart, start, size);
  if (demand) {
    cart->paused = true;
    cart->pending_start = line_start;
  }
  cart->stats.phone_requests++;
  prv_note_bank(&cart->stats.request_bank_mask, bank);
  PB_LOG("phone request bank %u fill %u size=%u%s", (unsigned)bank,
         (unsigned)offset, (unsigned)size, demand ? "" : " prefetch");
  return true;
}

bool pb_cart_ensure_addr(PbCart *cart, uint32_t addr) {
  if (addr >= cart->rom_size) {
    pb_cart_set_error(cart, "address outside ROM");
    return false;
  }

  uint32_t start = prv_slot_start(addr);
  uint16_t bank = prv_slot_bank(start);
  const PbCartSlot *slot = prv_find_const_slot(cart, start);
  if (slot && slot->valid) {
    return true;
  }

  cart->stats.misses++;
  cart->stats.last_miss_bank = bank;
  if (cart->mode == PB_CART_MODE_PHONE) {
    prv_request_phone_fill(cart, start, true);
    return false;
  }
  return prv_load_fill(cart, start);
}

bool pb_cart_ensure_bank(PbCart *cart, uint16_t bank) {
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE;
  if (start >= cart->rom_size) {
    pb_cart_set_error(cart, "bank outside ROM");
    return false;
  }
  return pb_cart_ensure_addr(cart, start);
}

bool pb_cart_ensure_fixed_bank(PbCart *cart) {
  for (uint32_t start = 0; start < PB_CART_BANK_SIZE && start < cart->rom_size;
       start += PB_CART_LINE_SIZE) {
    const PbCartSlot *slot = prv_find_const_slot(cart, start);
    if (slot && slot->valid) {
      continue;
    }
    if (!pb_cart_ensure_addr(cart, start)) {
      return false;
    }
  }
  return true;
}

uint8_t pb_cart_read(PbCart *cart, uint32_t addr) {
  if (addr >= cart->rom_size) {
    return 0xFF;
  }

  uint32_t start = prv_slot_start(addr);
  uint16_t bank = prv_slot_bank(start);
  uint16_t offset = (uint16_t)(addr - start);
  PbCartSlot *slot = prv_find_slot(cart, start);
  if (!slot || !slot->valid) {
    cart->stats.misses++;
    cart->stats.last_miss_bank = bank;
    if (cart->mode == PB_CART_MODE_PHONE) {
      cart->read_faulted = true;
      cart->read_fault_start = start;
      prv_request_phone_fill(cart, start, true);
      return 0xFF;
    }
    if (!prv_load_fill(cart, start)) {
      return 0xFF;
    }
    slot = prv_find_slot(cart, start);
  }

  if (!slot || !slot->valid || offset >= slot->size) {
    return 0xFF;
  }
  slot->last_used = ++cart->tick;
  cart->stats.hits++;
  return slot->data[offset];
}

bool pb_cart_has_bank(const PbCart *cart, uint16_t bank) {
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE;
  const PbCartSlot *slot = prv_find_const_slot(cart, start);
  return slot && slot->valid;
}

bool pb_cart_prefetch_addr(PbCart *cart, uint32_t addr) {
  if (cart->mode != PB_CART_MODE_PHONE || addr >= cart->rom_size) {
    return false;
  }
  uint32_t start = prv_slot_start(addr);
  const PbCartSlot *slot = prv_find_const_slot(cart, start);
  if (slot && (slot->valid || slot->loading)) {
    return false;
  }
  return prv_request_phone_fill(cart, start, false);
}

void pb_cart_set_active_bank(PbCart *cart, uint16_t bank) {
  cart->active_bank = bank < cart->bank_count ? bank : PB_CART_ACTIVE_BANK_NONE;
}

bool pb_cart_paused(const PbCart *cart) {
  return cart->paused || cart->failed;
}

bool pb_cart_read_faulted(const PbCart *cart) {
  return cart->read_faulted;
}

void pb_cart_clear_read_fault(PbCart *cart) {
  cart->read_faulted = false;
  cart->read_fault_start = UINT32_MAX;
}

void pb_cart_resume(PbCart *cart) {
  if (!cart->failed) {
    cart->paused = false;
    cart->pending_start = UINT32_MAX;
    pb_cart_clear_read_fault(cart);
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

bool pb_cart_phone_begin(PbCart *cart, uint16_t bank, uint16_t offset, uint16_t size) {
  if (cart->mode != PB_CART_MODE_PHONE || bank >= cart->bank_count ||
      offset >= PB_CART_BANK_SIZE || size > PB_CART_FILL_SIZE ||
      offset + size > PB_CART_BANK_SIZE) {
    return false;
  }
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE + offset;
  prv_prepare_loading_fill(cart, start, size);
  return true;
}

bool pb_cart_phone_data(PbCart *cart, uint16_t bank, uint16_t offset, const uint8_t *data,
                        uint16_t len) {
  uint32_t abs = (uint32_t)bank * PB_CART_BANK_SIZE + offset;
  uint16_t copied = 0;
  while (copied < len) {
    uint32_t cur = abs + copied;
    uint32_t start = prv_slot_start(cur);
    PbCartSlot *slot = prv_find_slot(cart, start);
    if (!slot || !slot->loading || cur < (uint32_t)slot->start ||
        cur >= (uint32_t)slot->start + slot->size) {
      return false;
    }
    uint16_t slot_offset = (uint16_t)(cur - (uint32_t)slot->start);
    uint16_t room = (uint16_t)(slot->size - slot_offset);
    uint16_t chunk = (uint16_t)(len - copied);
    if (chunk > room) {
      chunk = room;
    }
    memcpy(slot->data + slot_offset, data + copied, chunk);
    uint16_t end = slot_offset + chunk;
    if (end > slot->received) {
      slot->received = end;
    }
    slot->last_used = ++cart->tick;
    copied += chunk;
  }
  return true;
}

bool pb_cart_phone_end(PbCart *cart, uint16_t bank, uint16_t offset, uint16_t size) {
  uint32_t start = (uint32_t)bank * PB_CART_BANK_SIZE + offset;
  uint16_t fill_size = size ? size : prv_fill_size(cart, start);
  uint32_t end = start + fill_size;
  for (uint32_t line_start = start; line_start < end; line_start += PB_CART_LINE_SIZE) {
    PbCartSlot *slot = prv_find_slot(cart, line_start);
    if (!slot || !slot->loading) {
      return false;
    }
    if (slot->received < slot->size) {
      cart->stats.failed_loads++;
      pb_cart_set_error(cart, "incomplete phone fill");
      return false;
    }
  }
  for (uint32_t line_start = start; line_start < end; line_start += PB_CART_LINE_SIZE) {
    PbCartSlot *slot = prv_find_slot(cart, line_start);
    slot->valid = true;
    slot->loading = false;
    slot->last_used = ++cart->tick;
  }
  cart->stats.loads++;
  cart->stats.last_load_bank = bank;
  prv_note_bank(&cart->stats.load_bank_mask, bank);
  PB_LOG("phone bank %u fill %u loaded size=%u", (unsigned)bank,
         (unsigned)offset, (unsigned)fill_size);
  const PbCartSlot *pending = prv_find_const_slot(cart, cart->pending_start);
  if ((cart->pending_start >= start && cart->pending_start < end) ||
      (pending && pending->valid) || !prv_has_loading_slots(cart)) {
    pb_cart_resume(cart);
  }
  return true;
}
