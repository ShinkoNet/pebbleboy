#ifndef PB_GB_AUDIO_H
#define PB_GB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void pb_audio_init(void);
void pb_audio_set_enabled(bool enabled);
void pb_audio_pump(void);
void pb_audio_deinit(void);
bool pb_audio_enabled(void);
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

#endif
