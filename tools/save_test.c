#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_save.h"

typedef struct {
  bool present[PB_SAVE_MAX_CHUNKS];
  uint32_t keys[PB_SAVE_MAX_CHUNKS];
  uint16_t sizes[PB_SAVE_MAX_CHUNKS];
  uint8_t data[PB_SAVE_MAX_CHUNKS][PB_SAVE_CHUNK_SIZE];
  uint16_t writes;
} MockStore;

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "save test failed: %s\n", message);
    exit(1);
  }
}

static int mock_find(const MockStore *store, uint32_t key) {
  for (int i = 0; i < (int)PB_SAVE_MAX_CHUNKS; i++) {
    if (store->present[i] && store->keys[i] == key) {
      return i;
    }
  }
  return -1;
}

static int mock_read(uint32_t key, void *data, size_t size, void *context) {
  MockStore *store = context;
  int slot = mock_find(store, key);
  if (slot < 0 || store->sizes[slot] != size) {
    return -9;
  }
  memcpy(data, store->data[slot], size);
  return (int)size;
}

static int mock_write(uint32_t key, const void *data, size_t size, void *context) {
  MockStore *store = context;
  int slot = mock_find(store, key);
  if (slot < 0) {
    for (int i = 0; i < (int)PB_SAVE_MAX_CHUNKS; i++) {
      if (!store->present[i]) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0 || size > PB_SAVE_CHUNK_SIZE) {
    return -6;
  }
  store->present[slot] = true;
  store->keys[slot] = key;
  store->sizes[slot] = (uint16_t)size;
  memcpy(store->data[slot], data, size);
  store->writes++;
  return (int)size;
}

int main(void) {
  MockStore store = {0};
  uint8_t data[PB_SAVE_MAX_SIZE];
  PbSave save;
  expect(pb_save_init(&save, data, sizeof(data), 0x1234, mock_read, mock_write,
                      &store),
         "initialization failed");
  expect(save.restored_chunks == 0, "empty store unexpectedly restored data");
  expect(save.read_ops == PB_SAVE_MAX_CHUNKS && save.read_bytes == 0,
         "empty-store read accounting mismatch");
  expect(pb_save_read(&save, 0) == 0xFF &&
             pb_save_read(&save, sizeof(data) - 1) == 0xFF,
         "fresh save was not erased to 0xff");
  expect(!pb_save_write(&save, 42, 0xFF), "unchanged byte became dirty");

  expect(pb_save_write(&save, 0, 0x42), "first write was ignored");
  expect(pb_save_write(&save, 255, 0x43), "same-chunk write was ignored");
  expect(pb_save_write(&save, 256, 0x44), "second-chunk write was ignored");
  expect(pb_save_write(&save, sizeof(data) - 1, 0x45), "last write was ignored");
  expect(pb_save_dirty_count(&save) == 3, "dirty chunk count mismatch");
  expect(pb_save_flush_all(&save) == 3, "dirty chunks did not flush");
  expect(store.writes == 3 && pb_save_dirty_count(&save) == 0,
         "flush wrote the wrong number of chunks");
  expect(save.write_ops == 3 && save.write_bytes == 3 * PB_SAVE_CHUNK_SIZE,
         "write accounting mismatch");

  uint8_t restored[PB_SAVE_MAX_SIZE];
  PbSave loaded;
  expect(pb_save_init(&loaded, restored, sizeof(restored), 0x1234, mock_read,
                      mock_write, &store),
         "restore initialization failed");
  expect(loaded.restored_chunks == 3, "restore chunk count mismatch");
  expect(loaded.read_ops == PB_SAVE_MAX_CHUNKS &&
             loaded.read_bytes == 3 * PB_SAVE_CHUNK_SIZE,
         "restore read accounting mismatch");
  expect(pb_save_read(&loaded, 0) == 0x42 && pb_save_read(&loaded, 255) == 0x43 &&
             pb_save_read(&loaded, 256) == 0x44 &&
             pb_save_read(&loaded, sizeof(restored) - 1) == 0x45,
         "restored values did not match");
  expect(pb_save_dirty_count(&loaded) == 0, "restored save started dirty");

  uint8_t other_data[PB_SAVE_MAX_SIZE];
  PbSave other;
  expect(pb_save_init(&other, other_data, sizeof(other_data), 0x5678, mock_read,
                      mock_write, &store),
         "second ROM initialization failed");
  expect(other.restored_chunks == 0 && pb_save_read(&other, 0) == 0xFF,
         "different ROM checksum reused another save");

  printf("save test passed chunks=%u writes=%u\n", (unsigned)loaded.restored_chunks,
         (unsigned)store.writes);
  return 0;
}
