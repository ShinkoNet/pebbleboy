#include "gb_store.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define STORE_SLOTS 64u
#define META_KEY 0x61000000u
#define DATA_KEY 0x62000000u
#define LEGACY_KEY 0x50000000u
#define LEGACY_RTC_KEY 0x52540000u
#define STORE_MAGIC 0x50425331u

typedef struct {
  uint32_t magic;
  uint32_t ram_size;
  uint8_t digest[20];
  uint8_t bank;
  uint8_t has_save;
  uint8_t reserved[2];
  uint64_t updated_at;
} StoreMeta;
static StoreMeta s_meta;
static uint32_t s_slot;
static bool s_open;
static char s_id[PB_STORE_ID_SIZE];

static uint32_t rol(uint32_t n, unsigned b) { return (n << b) | (n >> (32 - b)); }
static void hash_block(uint32_t h[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (unsigned i = 0; i < 16; ++i) {
    const uint8_t *p = block + 4 * i;
    w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
  }
  for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
  uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
  for (unsigned i=0; i<80; ++i) {
    uint32_t f, k;
    if (i<20) { f=(b&c)|(~b&d); k=0x5a827999; }
    else if (i<40) { f=b^c^d; k=0x6ed9eba1; }
    else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
    else { f=b^c^d; k=0xca62c1d6; }
    uint32_t t=rol(a,5)+f+e+k+w[i];
    e=d; d=c; c=rol(b,30); b=a; a=t;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}
static bool hash_cart(PbCart *cart, uint8_t digest[20]) {
  uint32_t h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
  uint8_t data[512];
  /* Supported cartridges contain whole 16 KiB banks, hence whole SHA blocks. */
  if (!cart->rom_size || cart->rom_size % 512) return false;
  for (uint32_t offset=0; offset<cart->rom_size; offset+=sizeof(data)) {
    int got=-1;
#ifdef PEBBLEBOY_APP_BLOB
    if (cart->mode==PB_CART_MODE_BLOB) got=app_blob_read(offset,data,sizeof(data));
#endif
    if (cart->mode==PB_CART_MODE_RESOURCE)
      got=(int)resource_load_byte_range(cart->resource,offset,data,sizeof(data));
    if (got!=(int)sizeof(data)) return false;
    for (unsigned i=0; i<sizeof(data); i+=64) hash_block(h,data+i);
  }
  memset(data,0,64);
  data[0]=0x80;
  uint32_t bits=cart->rom_size*8u;
  for (unsigned i=0;i<4;++i) data[63-i]=(uint8_t)(bits>>(8*i));
  hash_block(h,data);
  for (unsigned i=0;i<20;++i) digest[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
  return true;
}
static uint32_t data_key(uint8_t bank, uint32_t chunk) {
  return DATA_KEY | (s_slot << 10) | ((uint32_t)bank << 8) | chunk;
}
static int read_bank(uint8_t bank, uint32_t chunk, void *data, size_t size) {
  uint32_t key=data_key(bank,chunk);
  if (!persist_exists(key)) { memset(data,0xff,size); return (int)size; }
  return persist_read_data(key,data,size);
}
bool pb_store_open(PbCart *cart, uint16_t checksum, uint32_t ram_size) {
  s_open=false;
  if (ram_size>PB_SAVE_MAX_SIZE) return false;
  uint8_t digest[20];
  if (!hash_cart(cart,digest)) return false;
  for (unsigned i=0;i<20;++i) snprintf(s_id+2*i,3,"%02x",digest[i]);
  uint32_t free_slot=STORE_SLOTS;
  for (s_slot=0;s_slot<STORE_SLOTS;++s_slot) {
    if (!persist_exists(META_KEY+s_slot)) {
      if (free_slot==STORE_SLOTS) free_slot=s_slot;
      continue;
    }
    StoreMeta meta = {0};
    int got = persist_read_data(META_KEY+s_slot,&meta,sizeof(meta));
    if ((got != 32 && got != (int)sizeof(meta)) || meta.magic!=STORE_MAGIC || meta.bank>1) return false;
    // Earlier preview records had no action time. Do not invent a new one.
    if (got == 32) meta.has_save = 1;
    if (!memcmp(meta.digest,digest,20)) {
      if (meta.ram_size!=ram_size) return false;
      s_meta=meta; s_open=true; return true;
    }
  }
  if (free_slot==STORE_SLOTS) return false;
  s_slot=free_slot;
  memset(&s_meta,0,sizeof(s_meta));
  s_meta.magic=STORE_MAGIC; s_meta.ram_size=ram_size;
  memcpy(s_meta.digest,digest,20);
  /* Copy old saves before publishing the new identity. Keep legacy keys intact
   * so an interrupted migration or a downgrade cannot destroy the old save. */
  uint8_t data[PB_SAVE_CHUNK_SIZE];
  for (uint32_t offset=0;offset<ram_size;offset+=sizeof(data)) {
    size_t size=ram_size-offset < sizeof(data) ? ram_size-offset : sizeof(data);
    uint32_t key=LEGACY_KEY|((uint32_t)checksum<<8)|(offset/PB_SAVE_CHUNK_SIZE);
    memset(data,0xff,size);
    if (persist_exists(key)) {
      if (persist_read_data(key,data,size)!=(int)size) return false;
      s_meta.has_save = 1;
    }
    if (persist_write_data(data_key(0,offset/PB_SAVE_CHUNK_SIZE),data,size)!=(int)size) return false;
  }
  memset(data,0,PB_STORE_RTC_SIZE);
  uint32_t rtc=LEGACY_RTC_KEY|checksum;
  if (persist_exists(rtc) && persist_read_data(rtc,data,PB_STORE_RTC_SIZE)!=PB_STORE_RTC_SIZE) return false;
  if (persist_write_data(data_key(0,128),data,PB_STORE_RTC_SIZE)!=PB_STORE_RTC_SIZE) return false;
  if (persist_write_data(META_KEY+s_slot,&s_meta,sizeof(s_meta))!=(int)sizeof(s_meta)) return false;
  s_open=true;
  return true;
}
const char *pb_store_id(void) { return s_open ? s_id : ""; }
uint32_t pb_store_ram_size(void) { return s_meta.ram_size; }
int pb_store_read(uint32_t key, void *data, size_t size, void *context) {
  (void)context;
  return s_open ? read_bank(s_meta.bank,key&0xffu,data,size) : -1;
}
int pb_store_write(uint32_t key, const void *data, size_t size, void *context) {
  (void)context;
  if (!s_open) return -1;
  int written = persist_write_data(data_key(s_meta.bank,key&0xffu),data,size);
  if (written != (int)size || (key & 0xffu) == 128) return written;
  // Record the emulator's write action, never a .sav file's filesystem dates.
  time_t seconds;
  uint16_t milliseconds;
  time_ms(&seconds, &milliseconds);
  uint64_t now = seconds > 0 ? (uint64_t)seconds * 1000u + milliseconds : 1;
  StoreMeta next = s_meta;
  next.updated_at = now > s_meta.updated_at ? now : s_meta.updated_at + 1;
  next.has_save = 1;
  if (persist_write_data(META_KEY+s_slot,&next,sizeof(next)) != (int)sizeof(next)) return -1;
  s_meta = next;
  return written;
}
int pb_store_read_rtc(void *data) { return pb_store_read(128,data,PB_STORE_RTC_SIZE,NULL); }
int pb_store_write_rtc(const void *data) { return pb_store_write(128,data,PB_STORE_RTC_SIZE,NULL); }
int pb_store_read_chunk(uint32_t offset, void *data, size_t size) {
  if (offset%PB_SAVE_CHUNK_SIZE || offset+size>s_meta.ram_size) return -1;
  return pb_store_read(offset/PB_SAVE_CHUNK_SIZE,data,size,NULL);
}
static bool stage_value(uint32_t chunk, const void *data, size_t size) {
  uint32_t key = data_key(s_meta.bank ^ 1u, chunk);
  uint8_t readback[PB_SAVE_CHUNK_SIZE];
  return persist_write_data(key, data, size) == (int)size &&
    persist_read_data(key, readback, size) == (int)size && !memcmp(data, readback, size);
}
bool pb_store_stage(uint32_t offset, const void *data, size_t size) {
  return s_open && offset%PB_SAVE_CHUNK_SIZE==0 && size>0 && size<=PB_SAVE_CHUNK_SIZE &&
    offset+size<=s_meta.ram_size &&
    stage_value(offset/PB_SAVE_CHUNK_SIZE,data,size);
}
bool pb_store_stage_rtc(const void *data) {
  return s_open && stage_value(128,data,PB_STORE_RTC_SIZE);
}
uint64_t pb_store_updated_at(void) { return s_meta.updated_at; }
bool pb_store_has_save(void) { return s_meta.has_save != 0; }
bool pb_store_commit(uint64_t updated_at) {
  StoreMeta next=s_meta;
  next.bank^=1u;
  next.has_save = 1;
  next.updated_at = updated_at;
  /* A single persisted value activates the fully validated staged generation. */
  if (!s_open || persist_write_data(META_KEY+s_slot,&next,sizeof(next))!=(int)sizeof(next)) return false;
  s_meta=next;
  return true;
}
uint32_t pb_store_crc32(uint32_t crc,const uint8_t *data,size_t size) {
  while (size--) {
    crc^=*data++;
    for (unsigned i=0;i<8;++i) crc=(crc>>1)^((crc&1u)?0xedb88320u:0);
  }
  return crc;
}
