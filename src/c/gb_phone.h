#ifndef PB_GB_PHONE_H
#define PB_GB_PHONE_H

#include <stdbool.h>
#include <stdint.h>

#include "gb_cart.h"

enum {
  PB_CMD_ROM_INFO_REQUEST = 10,
  PB_CMD_ROM_INFO = 11,
  PB_CMD_ROM_BANK_REQUEST = 12,
  PB_CMD_ROM_BANK_BEGIN = 13,
  PB_CMD_ROM_BANK_DATA = 14,
  PB_CMD_ROM_BANK_END = 15,
  PB_CMD_ROM_ERROR = 16,
  PB_CMD_SRAM_LOAD_REQUEST = 20,
  PB_CMD_SRAM_LOAD_DATA = 21,
  PB_CMD_SRAM_SAVE = 22,
};

typedef enum {
  PB_PHONE_EVENT_INFO = 0,
  PB_PHONE_EVENT_BANK_READY,
  PB_PHONE_EVENT_ERROR,
} PbPhoneEventType;

typedef struct {
  PbPhoneEventType type;
  uint16_t bank;
  uint32_t size;
  uint8_t cart_type;
  char title[17];
  char status[64];
} PbPhoneEvent;

typedef void (*PbPhoneEventCb)(const PbPhoneEvent *event, void *context);

void gb_phone_init(PbCart *cart, PbPhoneEventCb event_cb, void *context);
void gb_phone_deinit(void);
bool gb_phone_request_info(void);
bool gb_phone_request_bank(uint16_t bank, uint16_t offset, uint16_t size);

#endif
