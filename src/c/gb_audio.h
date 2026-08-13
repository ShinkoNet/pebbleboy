#ifndef PB_GB_AUDIO_H
#define PB_GB_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum samples in one 60 Hz scheduling quantum at 16 kHz. The mixer
// alternates 266/267-sample buffers to produce exactly 16,000 samples/second.
#define PB_AUDIO_PUMP_SAMPLES 267u

void pb_audio_init(void);
void pb_audio_set_enabled(bool enabled);
void pb_audio_pump(void);
void pb_audio_pump_silence(void);
void pb_audio_suspend_stream(void);
void pb_audio_deinit(void);
bool pb_audio_enabled(void);
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

typedef struct {
  uint32_t pumps;
  uint32_t partial_writes;
  uint32_t stream_errors;
  uint32_t last_write_size;
  uint32_t generated_buffers;
  uint16_t last_nonzero_samples;
  uint16_t last_peak;
  uint8_t active_channels;
  uint8_t nr50;
  uint8_t nr51;
  uint8_t nr52;
} PbAudioStats;

const PbAudioStats *pb_audio_stats(void);

#ifdef PB_DESKTOP
const int16_t *pb_audio_debug_buffer(size_t *sample_count_out);
uint32_t pb_audio_debug_sample_rate(void);
#endif

#endif
