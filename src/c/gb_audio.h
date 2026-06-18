#ifndef PB_GB_AUDIO_H
#define PB_GB_AUDIO_H

#include <stdbool.h>

void pb_audio_init(void);
void pb_audio_pump(void);
void pb_audio_deinit(void);
bool pb_audio_enabled(void);

#endif

