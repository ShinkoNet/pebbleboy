#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef PB_DESKTOP
#include <pebble.h>

/* CloudPebble builds projects with its own stock wscript, so build-time
 * defines from this repository are not available there.  ROM-free builds are
 * the normal Pebbleboy target: select the blob path in source and synthesize
 * the seven CFW veneers when the SDK does not already declare that API.
 *
 * A per-ROM builder explicitly defines PEBBLEBOY_EMBEDDED_ROM_BUILD and uses
 * an ordinary immutable resource instead. */
#ifndef PEBBLEBOY_EMBEDDED_ROM_BUILD
#ifndef PEBBLEBOY_APP_BLOB
#define PEBBLEBOY_APP_BLOB 1
#endif
#if !defined(_PBL_API_EXISTS_app_blob_get_info) && \
    !defined(PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE)
#define PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE 1
#endif
#endif
#endif

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
