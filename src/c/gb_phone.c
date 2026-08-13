#include "gb_phone.h"

#include <string.h>

#include <pebble.h>

extern uint32_t MESSAGE_KEY_PB_CMD;
extern uint32_t MESSAGE_KEY_PB_BANK;
extern uint32_t MESSAGE_KEY_PB_OFFSET;
extern uint32_t MESSAGE_KEY_PB_SIZE;
extern uint32_t MESSAGE_KEY_PB_DATA;
extern uint32_t MESSAGE_KEY_PB_TITLE;
extern uint32_t MESSAGE_KEY_PB_CART_TYPE;
extern uint32_t MESSAGE_KEY_PB_STATUS;
extern uint32_t MESSAGE_KEY_PB_CRC32;

#define INSTALL_REPLY_MAX_RETRIES 5u
/* Pebble's generated C table omits phone-info-only settings keys. */
#define MESSAGE_KEY_PB_AUDIO_FALLBACK 10010u
#define MESSAGE_KEY_PB_SCALE_FALLBACK 10011u

static PbPhoneEventCb s_event_cb;
static void *s_event_context;

typedef struct {
  bool active;
  uint8_t cmd;
  uint8_t retries;
  uint32_t offset;
} PbPhoneInstallReply;

static PbPhoneInstallReply s_install_reply;

static void prv_emit(PbPhoneEvent *event) {
  if (s_event_cb) {
    s_event_cb(event, s_event_context);
  }
}

static void prv_emit_error(const char *status) {
  PbPhoneEvent event;
  memset(&event, 0, sizeof(event));
  event.type = PB_PHONE_EVENT_ERROR;
  strncpy(event.status, status ? status : "phone error", sizeof(event.status) - 1);
  prv_emit(&event);
}

static bool prv_send_request(uint8_t cmd, uint16_t index) {
  if (s_install_reply.active) {
    return false;
  }

  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(out, MESSAGE_KEY_PB_CMD, cmd);
  if (cmd == PB_CMD_ROM_SELECT) {
    dict_write_uint16(out, MESSAGE_KEY_PB_BANK, index);
  }
  return app_message_outbox_send() == APP_MSG_OK;
}

bool gb_phone_request_info(void) {
  return prv_send_request(PB_CMD_ROM_INFO_REQUEST, 0);
}

bool gb_phone_request_rom_list(void) {
  return prv_send_request(PB_CMD_ROM_LIST_REQUEST, 0);
}

bool gb_phone_select_rom(uint16_t index) {
  return prv_send_request(PB_CMD_ROM_SELECT, index);
}

static bool prv_send_pending_install_reply(void) {
  if (!s_install_reply.active) {
    return false;
  }
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(out, MESSAGE_KEY_PB_CMD, s_install_reply.cmd);
  if (s_install_reply.cmd == PB_CMD_ROM_INSTALL_ACK) {
    dict_write_uint32(out, MESSAGE_KEY_PB_OFFSET, s_install_reply.offset);
  }
  return app_message_outbox_send() == APP_MSG_OK;
}

static bool prv_send_install_status(uint8_t cmd, uint32_t offset) {
  if (s_install_reply.active) {
    return false;
  }
  s_install_reply.active = true;
  s_install_reply.cmd = cmd;
  s_install_reply.offset = offset;
  if (prv_send_pending_install_reply()) {
    return true;
  }
  memset(&s_install_reply, 0, sizeof(s_install_reply));
  return false;
}

bool gb_phone_install_ack(uint32_t offset) {
  return prv_send_install_status(PB_CMD_ROM_INSTALL_ACK, offset);
}

bool gb_phone_install_done(void) {
  return prv_send_install_status(PB_CMD_ROM_INSTALL_DONE, 0);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  (void)context;
  Tuple *cmd = dict_find(iter, MESSAGE_KEY_PB_CMD);
  if (!cmd) {
    return;
  }

  PbPhoneEvent event;
  memset(&event, 0, sizeof(event));

  switch (cmd->value->uint8) {
    case PB_CMD_ROM_LIST_BEGIN: {
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      event.type = PB_PHONE_EVENT_ROM_LIST_BEGIN;
      event.size = size ? size->value->uint32 : 0;
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_LIST_ITEM: {
      Tuple *index = dict_find(iter, MESSAGE_KEY_PB_BANK);
      Tuple *title = dict_find(iter, MESSAGE_KEY_PB_TITLE);
      if (!index) {
        return;
      }
      event.type = PB_PHONE_EVENT_ROM_LIST_ITEM;
      event.bank = index->value->uint16;
      if (title) {
        strncpy(event.title, title->value->cstring, sizeof(event.title) - 1);
      }
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_LIST_END: {
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      event.type = PB_PHONE_EVENT_ROM_LIST_END;
      event.size = size ? size->value->uint32 : 0;
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_INFO: {
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      Tuple *title = dict_find(iter, MESSAGE_KEY_PB_TITLE);
      Tuple *cart_type = dict_find(iter, MESSAGE_KEY_PB_CART_TYPE);
      Tuple *audio = dict_find(iter, MESSAGE_KEY_PB_AUDIO_FALLBACK);
      Tuple *scale = dict_find(iter, MESSAGE_KEY_PB_SCALE_FALLBACK);
      Tuple *crc32 = dict_find(iter, MESSAGE_KEY_PB_CRC32);
      if (!size) {
        return;
      }
      event.type = PB_PHONE_EVENT_INFO;
      event.size = size->value->uint32;
      event.cart_type = cart_type ? cart_type->value->uint8 : 0;
      event.crc32 = crc32 ? crc32->value->uint32 : 0;
      event.video_scale = scale ? scale->value->uint8 : 0;
      event.audio_enabled = !audio || audio->value->uint8;
      if (title) {
        strncpy(event.title, title->value->cstring, sizeof(event.title) - 1);
      }
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_INSTALL_DATA: {
      Tuple *offset = dict_find(iter, MESSAGE_KEY_PB_OFFSET);
      Tuple *data = dict_find(iter, MESSAGE_KEY_PB_DATA);
      if (!offset || !data) {
        return;
      }
      event.type = PB_PHONE_EVENT_INSTALL_DATA;
      event.offset = offset->value->uint32;
      event.data = data->value->data;
      event.data_len = data->length;
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_INSTALL_END: {
      Tuple *crc32 = dict_find(iter, MESSAGE_KEY_PB_CRC32);
      if (!crc32) {
        return;
      }
      event.type = PB_PHONE_EVENT_INSTALL_END;
      event.crc32 = crc32->value->uint32;
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_ERROR: {
      Tuple *status = dict_find(iter, MESSAGE_KEY_PB_STATUS);
      event.type = PB_PHONE_EVENT_ERROR;
      if (status) {
        strncpy(event.status, status->value->cstring, sizeof(event.status) - 1);
      } else {
        strncpy(event.status, "phone ROM error", sizeof(event.status) - 1);
      }
      prv_emit(&event);
      break;
    }
    default:
      break;
  }
}

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  (void)iter;
  (void)context;
  if (s_install_reply.active) {
    memset(&s_install_reply, 0, sizeof(s_install_reply));
  }
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  (void)iter;
  (void)context;
  APP_LOG(APP_LOG_LEVEL_WARNING, "phone outbox failed: %d", (int)reason);
  if (s_install_reply.active) {
    if (++s_install_reply.retries <= INSTALL_REPLY_MAX_RETRIES &&
        prv_send_pending_install_reply()) {
      return;
    }
    memset(&s_install_reply, 0, sizeof(s_install_reply));
    prv_emit_error("install reply failed");
    return;
  }
}

void gb_phone_init(PbCart *cart, PbPhoneEventCb event_cb, void *context) {
  (void)cart;
  s_event_cb = event_cb;
  s_event_context = context;
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  /* A ROM data message contains 512 bytes plus command and offset tuples.
   * Outbound acknowledgements contain only a command and optional offset. */
  app_message_open(600, 64);
}

void gb_phone_deinit(void) {
  app_message_deregister_callbacks();
  memset(&s_install_reply, 0, sizeof(s_install_reply));
  s_event_cb = NULL;
  s_event_context = NULL;
}
