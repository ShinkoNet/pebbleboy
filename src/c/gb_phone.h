#ifndef PB_GB_PHONE_H
#define PB_GB_PHONE_H

#include <stdbool.h>
#include <stdint.h>

#include "gb_cart.h"

enum {
  PB_CMD_ROM_INFO_REQUEST = 10,
  PB_CMD_ROM_INFO = 11,
  PB_CMD_ROM_ERROR = 16,
  PB_CMD_ROM_LIST_REQUEST = 30,
  PB_CMD_ROM_LIST_BEGIN = 31,
  PB_CMD_ROM_LIST_ITEM = 32,
  PB_CMD_ROM_LIST_END = 33,
  PB_CMD_ROM_SELECT = 34,
  PB_CMD_ROM_INSTALL_DATA = 40,
  PB_CMD_ROM_INSTALL_ACK = 41,
  PB_CMD_ROM_INSTALL_END = 42,
  PB_CMD_ROM_INSTALL_DONE = 43,
};

typedef enum {
  PB_PHONE_EVENT_INFO = 0,
  PB_PHONE_EVENT_ROM_LIST_BEGIN,
  PB_PHONE_EVENT_ROM_LIST_ITEM,
  PB_PHONE_EVENT_ROM_LIST_END,
  PB_PHONE_EVENT_INSTALL_DATA,
  PB_PHONE_EVENT_INSTALL_END,
  PB_PHONE_EVENT_ERROR,
} PbPhoneEventType;

typedef struct {
  PbPhoneEventType type;
  uint16_t bank;
  uint32_t offset;
  uint32_t size;
  uint32_t crc32;
  uint8_t cart_type;
  uint8_t video_scale;
  bool audio_enabled;
  const uint8_t *data;
  uint16_t data_len;
  char title[17];
  char status[64];
} PbPhoneEvent;

typedef void (*PbPhoneEventCb)(const PbPhoneEvent *event, void *context);

void gb_phone_init(PbCart *cart, PbPhoneEventCb event_cb, void *context);
void gb_phone_deinit(void);
bool gb_phone_request_info(void);
bool gb_phone_request_rom_list(void);
bool gb_phone_select_rom(uint16_t index);
bool gb_phone_install_ack(uint32_t offset);
bool gb_phone_install_done(void);

#endif
