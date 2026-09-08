#ifndef PB_GB_PHONE_H
#define PB_GB_PHONE_H

#include <stdbool.h>
#include <stdint.h>

#include "gb_cart.h"

enum {
  PB_CMD_SAVE_REQUEST = 50,
  PB_CMD_SAVE_INFO = 51,
  PB_CMD_SAVE_READ = 52,
  PB_CMD_SAVE_DATA = 53,
  PB_CMD_SAVE_BEGIN = 54,
  PB_CMD_SAVE_WRITE = 55,
  PB_CMD_SAVE_COMMIT = 56,
  PB_CMD_SAVE_ACK = 57,
  PB_CMD_SAVE_ABORT = 58,
  PB_CMD_SAVE_ERROR = 59,
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
  PB_CMD_ROM_LAUNCH = 44,
  PB_CMD_SETTINGS_OPEN = 45,
  PB_CMD_SETTINGS_CLOSE = 46,
  PB_CMD_ROM_LIST_LAUNCH = 47,
};

typedef enum {
  PB_PHONE_EVENT_INFO = 0,
  PB_PHONE_EVENT_ROM_LIST_BEGIN,
  PB_PHONE_EVENT_ROM_LIST_ITEM,
  PB_PHONE_EVENT_ROM_LIST_END,
  PB_PHONE_EVENT_INSTALL_DATA,
  PB_PHONE_EVENT_INSTALL_END,
  PB_PHONE_EVENT_ERROR,
  PB_PHONE_EVENT_SAVE,
  PB_PHONE_EVENT_SETTINGS,
} PbPhoneEventType;

typedef struct {
  PbPhoneEventType type;
  uint32_t bank;
  uint8_t command;
  char rom_id[41];
  uint32_t offset;
  uint32_t size;
  uint32_t crc32;
  uint64_t updated_at;
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
bool gb_phone_save_send(uint8_t cmd, uint32_t token, uint32_t offset,
                        uint32_t size, uint32_t crc, const char *id,
                        const char *title, const void *data, size_t length,
                        uint64_t updated_at, bool filled);
bool gb_phone_request_info(void);
bool gb_phone_request_rom_list(void);
bool gb_phone_select_rom(uint16_t index);
bool gb_phone_install_ack(uint32_t offset);
bool gb_phone_install_done(void);

#endif
