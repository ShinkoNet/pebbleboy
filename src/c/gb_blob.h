#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE
typedef struct {
  uint32_t size;
  uint32_t crc32;
} AppBlobInfo;

int32_t app_blob_get_info(AppBlobInfo *info_out);
size_t app_blob_get_free_size(void);
int32_t app_blob_begin(uint32_t size);
int app_blob_write(uint32_t offset, const void *data, size_t size);
int32_t app_blob_commit(uint32_t expected_crc32);
int app_blob_read(uint32_t offset, void *data, size_t size);
int32_t app_blob_delete(void);
#endif
