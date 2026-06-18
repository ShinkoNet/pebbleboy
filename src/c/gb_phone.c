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
extern uint32_t MESSAGE_KEY_PB_SRAM_SIZE;

#define SRAM_SAVE_CHUNK_SIZE 512u
#define SRAM_SAVE_MAX_RETRIES 5u
/* Pebble's generated C table omits phone-info-only settings keys. */
#define MESSAGE_KEY_PB_AUDIO_FALLBACK 10010u
#define MESSAGE_KEY_PB_SCALE_FALLBACK 10011u

static PbCart *s_cart;
static PbPhoneEventCb s_event_cb;
static void *s_event_context;

typedef struct {
  bool active;
  uint16_t bank;
  uint16_t size;
  uint16_t offset;
  uint16_t in_flight_size;
  uint32_t total_size;
  uint8_t retries;
  const uint8_t *data;
} PbPhoneSramSave;

static PbPhoneSramSave s_sram_save;

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

static bool prv_send_request(uint8_t cmd, uint16_t bank, uint16_t offset, uint16_t size) {
  if (s_sram_save.active) {
    return false;
  }

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

bool gb_phone_request_sram_load(uint16_t bank, uint16_t size, uint32_t total_size) {
  if (s_sram_save.active) {
    return false;
  }

  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(out, MESSAGE_KEY_PB_CMD, PB_CMD_SRAM_LOAD_REQUEST);
  dict_write_uint16(out, MESSAGE_KEY_PB_BANK, bank);
  dict_write_uint16(out, MESSAGE_KEY_PB_SIZE, size);
  dict_write_uint32(out, MESSAGE_KEY_PB_SRAM_SIZE, total_size);
  return app_message_outbox_send() == APP_MSG_OK;
}

static bool prv_send_sram_save_chunk(void) {
  if (!s_sram_save.active) {
    return false;
  }

  if (s_sram_save.offset >= s_sram_save.size) {
    PbPhoneEvent event;
    memset(&event, 0, sizeof(event));
    event.type = PB_PHONE_EVENT_SRAM_SAVE_DONE;
    event.bank = s_sram_save.bank;
    event.size = s_sram_save.size;
    s_sram_save.active = false;
    prv_emit(&event);
    return true;
  }

  uint16_t chunk = (uint16_t)(s_sram_save.size - s_sram_save.offset);
  if (chunk > SRAM_SAVE_CHUNK_SIZE) {
    chunk = SRAM_SAVE_CHUNK_SIZE;
  }

  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_uint8(out, MESSAGE_KEY_PB_CMD, PB_CMD_SRAM_SAVE);
  dict_write_uint16(out, MESSAGE_KEY_PB_BANK, s_sram_save.bank);
  dict_write_uint16(out, MESSAGE_KEY_PB_OFFSET, s_sram_save.offset);
  dict_write_uint16(out, MESSAGE_KEY_PB_SIZE, chunk);
  dict_write_uint32(out, MESSAGE_KEY_PB_SRAM_SIZE, s_sram_save.total_size);
  dict_write_data(out, MESSAGE_KEY_PB_DATA, s_sram_save.data + s_sram_save.offset, chunk);
  if (app_message_outbox_send() != APP_MSG_OK) {
    return false;
  }
  s_sram_save.in_flight_size = chunk;
  return true;
}

bool gb_phone_save_sram_bank(uint16_t bank, const uint8_t *data, uint16_t size,
                             uint32_t total_size) {
  if (s_sram_save.active || !data || !size) {
    return false;
  }

  memset(&s_sram_save, 0, sizeof(s_sram_save));
  s_sram_save.active = true;
  s_sram_save.bank = bank;
  s_sram_save.data = data;
  s_sram_save.size = size;
  s_sram_save.total_size = total_size;
  if (!prv_send_sram_save_chunk()) {
    s_sram_save.active = false;
    return false;
  }
  return true;
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
      Tuple *audio = dict_find(iter, MESSAGE_KEY_PB_AUDIO_FALLBACK);
      Tuple *scale = dict_find(iter, MESSAGE_KEY_PB_SCALE_FALLBACK);
      if (!size) {
        return;
      }
      event.type = PB_PHONE_EVENT_INFO;
      event.size = size->value->uint32;
      event.cart_type = cart_type ? cart_type->value->uint8 : 0;
      event.video_scale = scale ? scale->value->uint8 : 0;
      event.audio_enabled = !audio || audio->value->uint8;
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
        event.offset = offset ? offset->value->uint16 : 0;
        event.size = size->value->uint16;
        prv_emit(&event);
      }
      break;
    }
    case PB_CMD_SRAM_LOAD_DATA: {
      Tuple *bank = dict_find(iter, MESSAGE_KEY_PB_BANK);
      Tuple *offset = dict_find(iter, MESSAGE_KEY_PB_OFFSET);
      Tuple *data = dict_find(iter, MESSAGE_KEY_PB_DATA);
      if (bank && offset && data) {
        event.type = PB_PHONE_EVENT_SRAM_LOAD_DATA;
        event.bank = bank->value->uint16;
        event.offset = offset->value->uint16;
        event.data = data->value->data;
        event.data_len = data->length;
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

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  (void)iter;
  (void)context;
  if (!s_sram_save.active) {
    return;
  }

  s_sram_save.offset += s_sram_save.in_flight_size;
  s_sram_save.in_flight_size = 0;
  s_sram_save.retries = 0;
  if (!prv_send_sram_save_chunk()) {
    s_sram_save.active = false;
    prv_emit_error("SRAM save failed");
  }
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  (void)iter;
  (void)context;
  APP_LOG(APP_LOG_LEVEL_WARNING, "phone outbox failed: %d", (int)reason);
  if (!s_sram_save.active) {
    return;
  }
  if (++s_sram_save.retries <= SRAM_SAVE_MAX_RETRIES && prv_send_sram_save_chunk()) {
    return;
  }
  s_sram_save.active = false;
  prv_emit_error("SRAM save failed");
}

void gb_phone_init(PbCart *cart, PbPhoneEventCb event_cb, void *context) {
  s_cart = cart;
  s_event_cb = event_cb;
  s_event_context = context;
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(1024, 1024);
}

void gb_phone_deinit(void) {
  app_message_deregister_callbacks();
  memset(&s_sram_save, 0, sizeof(s_sram_save));
  s_cart = NULL;
  s_event_cb = NULL;
  s_event_context = NULL;
}
