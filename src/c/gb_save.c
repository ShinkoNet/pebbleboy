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

bool pb_save_select_window(PbSave *save, size_t addr) {
  if (!save || !save->data || addr >= save->size || !save->data_size) {
    return false;
  }

  size_t start = (addr / save->data_size) * save->data_size;
  if (save->window_start == start) {
    return true;
  }
  if (save->window_start != SIZE_MAX && pb_save_flush_all(save) < 0) {
    return false;
  }

  memset(save->data, 0xFF, save->data_size);
  save->window_start = start;
  if (!save->read_cb) {
    return true;
  }

  uint8_t stored[PB_SAVE_CHUNK_SIZE];
  uint16_t first_chunk = (uint16_t)(start / PB_SAVE_CHUNK_SIZE);
  size_t end = start + save->data_size;
  if (end > save->size) {
    end = save->size;
  }
  uint16_t end_chunk = (uint16_t)((end + PB_SAVE_CHUNK_SIZE - 1u) /
                                  PB_SAVE_CHUNK_SIZE);
  for (uint16_t chunk = first_chunk; chunk < end_chunk; chunk++) {
    size_t size_to_read = prv_chunk_size(save, chunk);
    save->read_ops++;
    int result = save->read_cb(pb_save_chunk_key(save, chunk), stored, size_to_read,
                               save->store_context);
    if (result == (int)size_to_read) {
      memcpy(save->data + (size_t)chunk * PB_SAVE_CHUNK_SIZE - start,
             stored, size_to_read);
      save->restored_chunks++;
      save->read_bytes += size_to_read;
    }
  }
  return true;
}

bool pb_save_init_window(PbSave *save, uint8_t *data, size_t size, size_t data_size,
                         uint16_t rom_checksum, PbSaveReadCb read_cb,
                         PbSaveWriteCb write_cb, void *store_context) {
  if (!save || !data || size == 0 || size > PB_SAVE_MAX_SIZE ||
      data_size == 0 || data_size > size || data_size % PB_SAVE_CHUNK_SIZE) {
    return false;
  }

  memset(save, 0, sizeof(*save));
  save->data = data;
  save->size = size;
  save->data_size = data_size;
  save->window_start = SIZE_MAX;
  save->key_base = PB_SAVE_KEY_PREFIX | ((uint32_t)rom_checksum << 8);
  save->read_cb = read_cb;
  save->write_cb = write_cb;
  save->store_context = store_context;

  return pb_save_select_window(save, 0);
}

bool pb_save_init(PbSave *save, uint8_t *data, size_t size, uint16_t rom_checksum,
                  PbSaveReadCb read_cb, PbSaveWriteCb write_cb, void *store_context) {
  return pb_save_init_window(save, data, size, size, rom_checksum,
                             read_cb, write_cb, store_context);
}

uint8_t pb_save_read(const PbSave *save, size_t addr) {
  if (!save || !save->data || addr >= save->size ||
      addr < save->window_start || addr - save->window_start >= save->data_size) {
    return 0xFF;
  }
  return save->data[addr - save->window_start];
}

bool pb_save_write(PbSave *save, size_t addr, uint8_t value) {
  if (!save || !save->data || addr >= save->size || addr < save->window_start ||
      addr - save->window_start >= save->data_size ||
      save->data[addr - save->window_start] == value) {
    return false;
  }
  save->data[addr - save->window_start] = value;
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
    size_t chunk_start = (size_t)chunk * PB_SAVE_CHUNK_SIZE;
    if (chunk_start < save->window_start ||
        chunk_start - save->window_start >= save->data_size) {
      return -1;
    }
    const uint8_t *data = save->data + chunk_start - save->window_start;
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
