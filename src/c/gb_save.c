#include "gb_save.h"

#include <string.h>

#define PB_SAVE_KEY_PREFIX 0x50000000u

static uint16_t prv_chunk_count(const PbSave *save) {
  return (uint16_t)((save->size + PB_SAVE_CHUNK_SIZE - 1u) / PB_SAVE_CHUNK_SIZE);
}

static size_t prv_chunk_size(const PbSave *save, uint16_t chunk) {
  size_t start = (size_t)chunk * PB_SAVE_CHUNK_SIZE;
  size_t left = save->size - start;
  return left < PB_SAVE_CHUNK_SIZE ? left : PB_SAVE_CHUNK_SIZE;
}

static bool prv_chunk_dirty(const PbSave *save, uint16_t chunk) {
  return (save->dirty[chunk >> 3] & (uint8_t)(1u << (chunk & 7u))) != 0;
}

static void prv_set_chunk_dirty(PbSave *save, uint16_t chunk, bool dirty) {
  uint8_t mask = (uint8_t)(1u << (chunk & 7u));
  if (dirty) {
    save->dirty[chunk >> 3] |= mask;
  } else {
    save->dirty[chunk >> 3] &= (uint8_t)~mask;
  }
}

uint32_t pb_save_chunk_key(const PbSave *save, uint16_t chunk) {
  return save->key_base | chunk;
}

bool pb_save_init(PbSave *save, uint8_t *data, size_t size, uint16_t rom_checksum,
                  PbSaveReadCb read_cb, PbSaveWriteCb write_cb, void *store_context) {
  if (!save || !data || size == 0 || size > PB_SAVE_MAX_SIZE) {
    return false;
  }

  memset(save, 0, sizeof(*save));
  memset(data, 0xFF, size);
  save->data = data;
  save->size = size;
  save->key_base = PB_SAVE_KEY_PREFIX | ((uint32_t)rom_checksum << 8);
  save->read_cb = read_cb;
  save->write_cb = write_cb;
  save->store_context = store_context;

  if (!read_cb) {
    return true;
  }

  uint8_t stored[PB_SAVE_CHUNK_SIZE];
  uint16_t chunks = prv_chunk_count(save);
  for (uint16_t chunk = 0; chunk < chunks; chunk++) {
    size_t size_to_read = prv_chunk_size(save, chunk);
    save->read_ops++;
    int result = read_cb(pb_save_chunk_key(save, chunk), stored, size_to_read,
                         store_context);
    if (result == (int)size_to_read) {
      memcpy(data + (size_t)chunk * PB_SAVE_CHUNK_SIZE, stored, size_to_read);
      save->restored_chunks++;
      save->read_bytes += size_to_read;
    }
  }
  return true;
}

uint8_t pb_save_read(const PbSave *save, size_t addr) {
  if (!save || !save->data || addr >= save->size) {
    return 0xFF;
  }
  return save->data[addr];
}

bool pb_save_write(PbSave *save, size_t addr, uint8_t value) {
  if (!save || !save->data || addr >= save->size || save->data[addr] == value) {
    return false;
  }
  save->data[addr] = value;
  prv_set_chunk_dirty(save, (uint16_t)(addr / PB_SAVE_CHUNK_SIZE), true);
  return true;
}

uint16_t pb_save_dirty_count(const PbSave *save) {
  if (!save || !save->data) {
    return 0;
  }
  uint16_t dirty = 0;
  uint16_t chunks = prv_chunk_count(save);
  for (uint16_t chunk = 0; chunk < chunks; chunk++) {
    if (prv_chunk_dirty(save, chunk)) {
      dirty++;
    }
  }
  return dirty;
}

int pb_save_flush_one(PbSave *save) {
  if (!save || !save->data || !save->write_cb) {
    return -1;
  }
  uint16_t chunks = prv_chunk_count(save);
  for (uint16_t checked = 0; checked < chunks; checked++) {
    uint16_t chunk = (uint16_t)((save->flush_cursor + checked) % chunks);
    if (!prv_chunk_dirty(save, chunk)) {
      continue;
    }
    size_t size_to_write = prv_chunk_size(save, chunk);
    const uint8_t *data = save->data + (size_t)chunk * PB_SAVE_CHUNK_SIZE;
    save->write_ops++;
    int result = save->write_cb(pb_save_chunk_key(save, chunk), data, size_to_write,
                                save->store_context);
    if (result != (int)size_to_write) {
      return -1;
    }
    prv_set_chunk_dirty(save, chunk, false);
    save->write_bytes += size_to_write;
    save->flush_cursor = (uint16_t)((chunk + 1u) % chunks);
    return 1;
  }
  return 0;
}

int pb_save_flush_all(PbSave *save) {
  int flushed = 0;
  while (pb_save_dirty_count(save)) {
    int result = pb_save_flush_one(save);
    if (result <= 0) {
      return result < 0 ? -1 : flushed;
    }
    flushed += result;
  }
  return flushed;
}
