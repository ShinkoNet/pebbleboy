#ifndef PB_GB_SAVE_H
#define PB_GB_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_SAVE_MAX_SIZE 32768u
#define PB_SAVE_CHUNK_SIZE 256u
#define PB_SAVE_MAX_CHUNKS (PB_SAVE_MAX_SIZE / PB_SAVE_CHUNK_SIZE)
#define PB_SAVE_DIRTY_BYTES ((PB_SAVE_MAX_CHUNKS + 7u) / 8u)

typedef int (*PbSaveReadCb)(uint32_t key, void *data, size_t size, void *context);
typedef int (*PbSaveWriteCb)(uint32_t key, const void *data, size_t size, void *context);

typedef struct {
  uint8_t *data;
  size_t size;
  size_t data_size;
  size_t window_start;
  uint32_t key_base;
  PbSaveReadCb read_cb;
  PbSaveWriteCb write_cb;
  void *store_context;
  uint8_t dirty[PB_SAVE_DIRTY_BYTES];
  uint16_t flush_cursor;
  uint16_t restored_chunks;
  uint16_t read_ops;
  uint16_t write_ops;
  uint32_t read_bytes;
  uint32_t write_bytes;
} PbSave;

bool pb_save_init(PbSave *save, uint8_t *data, size_t size, uint16_t rom_checksum,
                  PbSaveReadCb read_cb, PbSaveWriteCb write_cb, void *store_context);
bool pb_save_init_window(PbSave *save, uint8_t *data, size_t size, size_t data_size,
                         uint16_t rom_checksum, PbSaveReadCb read_cb,
                         PbSaveWriteCb write_cb, void *store_context);
bool pb_save_select_window(PbSave *save, size_t addr);
uint8_t pb_save_read(const PbSave *save, size_t addr);
bool pb_save_write(PbSave *save, size_t addr, uint8_t value);
uint16_t pb_save_dirty_count(const PbSave *save);
int pb_save_flush_one(PbSave *save);
int pb_save_flush_all(PbSave *save);
uint32_t pb_save_chunk_key(const PbSave *save, uint16_t chunk);

#endif
