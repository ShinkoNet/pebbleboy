#include "gb_blob.h"

#ifdef PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE
#include <pebble.h>

/* PebbleOS v4.33.1-pebbleboy2 app-blob jump-table offsets. Reuse an official
 * SDK veneer so metadata injection still patches its single table pointer. */
#define PB_DEFINE_CFW_VENEER(return_type, name, arguments, offset)            \
  __attribute__((naked)) return_type name arguments {                         \
    __asm volatile("push {r0, r1, r2, r3}\n"                                \
                   "movw r1, #" #offset "\n"                                \
                   "ldr r12, =app_message_open\n"                            \
                   "add.w r12, r12, #6\n"                                    \
                   "bx r12\n");                                              \
  }

PB_DEFINE_CFW_VENEER(int32_t, app_blob_begin, (uint32_t size), 2708)
PB_DEFINE_CFW_VENEER(int32_t, app_blob_commit, (uint32_t expected_crc32), 2712)
PB_DEFINE_CFW_VENEER(int32_t, app_blob_delete, (void), 2716)
PB_DEFINE_CFW_VENEER(size_t, app_blob_get_free_size, (void), 2720)
PB_DEFINE_CFW_VENEER(int32_t, app_blob_get_info, (AppBlobInfo * info_out), 2724)
PB_DEFINE_CFW_VENEER(int, app_blob_read,
                     (uint32_t offset, void *data, size_t size), 2728)
PB_DEFINE_CFW_VENEER(int, app_blob_write,
                     (uint32_t offset, const void *data, size_t size), 2732)
#endif
