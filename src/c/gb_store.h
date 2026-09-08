#ifndef PB_GB_STORE_H
#define PB_GB_STORE_H
#include "gb_cart.h"
#include "gb_save.h"
#define PB_STORE_RTC_SIZE 24u
#define PB_STORE_ID_SIZE 41u
/* Full ROM SHA-1 identifies content, not a URL or the 16-bit header checksum.
 * This is a cartridge identifier, not a cryptographic trust/authentication API. */
bool pb_store_open(PbCart *cart, uint16_t checksum, uint32_t ram_size);
const char *pb_store_id(void);
uint32_t pb_store_ram_size(void);
int pb_store_read(uint32_t key, void *data, size_t size, void *context);
int pb_store_write(uint32_t key, const void *data, size_t size, void *context);
int pb_store_read_rtc(void *data);
int pb_store_write_rtc(const void *data);
int pb_store_read_chunk(uint32_t offset, void *data, size_t size);
bool pb_store_stage(uint32_t offset, const void *data, size_t size);
bool pb_store_stage_rtc(const void *data);
bool pb_store_commit(uint64_t updated_at);
uint64_t pb_store_updated_at(void);
bool pb_store_has_save(void);
uint32_t pb_store_crc32(uint32_t crc, const uint8_t *data, size_t size);
#endif
