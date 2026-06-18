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

static PbCart *s_cart;
static PbPhoneEventCb s_event_cb;
static void *s_event_context;

static void prv_emit(PbPhoneEvent *event) {
  if (s_event_cb) {
    s_event_cb(event, s_event_context);
  }
}

static bool prv_send_request(uint8_t cmd, uint16_t bank, uint16_t offset, uint16_t size) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(out, MESSAGE_KEY_PB_CMD, cmd);
  if (cmd == PB_CMD_ROM_BANK_REQUEST) {
    dict_write_uint16(out, MESSAGE_KEY_PB_BANK, bank);
    dict_write_uint16(out, MESSAGE_KEY_PB_OFFSET, offset);
    dict_write_uint16(out, MESSAGE_KEY_PB_SIZE, size);
  }
  return app_message_outbox_send() == APP_MSG_OK;
}

bool gb_phone_request_info(void) {
  return prv_send_request(PB_CMD_ROM_INFO_REQUEST, 0, 0, 0);
}

bool gb_phone_request_bank(uint16_t bank, uint16_t offset, uint16_t size) {
  return prv_send_request(PB_CMD_ROM_BANK_REQUEST, bank, offset, size);
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
    case PB_CMD_ROM_INFO: {
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      Tuple *title = dict_find(iter, MESSAGE_KEY_PB_TITLE);
      Tuple *cart_type = dict_find(iter, MESSAGE_KEY_PB_CART_TYPE);
      if (!size) {
        return;
      }
      event.type = PB_PHONE_EVENT_INFO;
      event.size = size->value->uint32;
      event.cart_type = cart_type ? cart_type->value->uint8 : 0;
      if (title) {
        strncpy(event.title, title->value->cstring, sizeof(event.title) - 1);
      }
      prv_emit(&event);
      break;
    }
    case PB_CMD_ROM_BANK_BEGIN: {
      Tuple *bank = dict_find(iter, MESSAGE_KEY_PB_BANK);
      Tuple *offset = dict_find(iter, MESSAGE_KEY_PB_OFFSET);
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      if (s_cart && bank && size) {
        pb_cart_phone_begin(s_cart, bank->value->uint16,
                            offset ? offset->value->uint16 : 0,
                            size->value->uint16);
      }
      break;
    }
    case PB_CMD_ROM_BANK_DATA: {
      Tuple *bank = dict_find(iter, MESSAGE_KEY_PB_BANK);
      Tuple *offset = dict_find(iter, MESSAGE_KEY_PB_OFFSET);
      Tuple *data = dict_find(iter, MESSAGE_KEY_PB_DATA);
      if (s_cart && bank && offset && data) {
        pb_cart_phone_data(s_cart, bank->value->uint16, offset->value->uint16,
                           data->value->data, data->length);
      }
      break;
    }
    case PB_CMD_ROM_BANK_END: {
      Tuple *bank = dict_find(iter, MESSAGE_KEY_PB_BANK);
      Tuple *offset = dict_find(iter, MESSAGE_KEY_PB_OFFSET);
      Tuple *size = dict_find(iter, MESSAGE_KEY_PB_SIZE);
      if (s_cart && bank && size &&
          pb_cart_phone_end(s_cart, bank->value->uint16,
                            offset ? offset->value->uint16 : 0,
                            size->value->uint16)) {
        event.type = PB_PHONE_EVENT_BANK_READY;
        event.bank = bank->value->uint16;
        event.size = size->value->uint16;
        prv_emit(&event);
      }
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

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  (void)iter;
  (void)context;
  APP_LOG(APP_LOG_LEVEL_WARNING, "phone outbox failed: %d", (int)reason);
}

void gb_phone_init(PbCart *cart, PbPhoneEventCb event_cb, void *context) {
  s_cart = cart;
  s_event_cb = event_cb;
  s_event_context = context;
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(1024, 1024);
}

void gb_phone_deinit(void) {
  app_message_deregister_callbacks();
  s_cart = NULL;
  s_event_cb = NULL;
  s_event_context = NULL;
}
