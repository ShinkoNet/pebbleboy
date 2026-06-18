#include "gb_audio.h"

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

static bool s_enabled;

void pb_audio_init(void) {
  s_enabled = false;
}

void pb_audio_pump(void) {
}

void pb_audio_deinit(void) {
#ifndef PB_DESKTOP
  if (s_enabled) {
    speaker_stream_close();
  }
#endif
  s_enabled = false;
}

bool pb_audio_enabled(void) {
  return s_enabled;
}

