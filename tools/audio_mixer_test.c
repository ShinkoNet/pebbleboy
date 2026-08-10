#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gb_audio.h"

typedef struct {
  int min;
  int max;
  unsigned nonzero;
  unsigned transitions;
} AudioStats;

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static AudioStats stats_for_buffer(void) {
  size_t size = 0;
  const int16_t *buffer = pb_audio_debug_buffer(&size);
  AudioStats stats = {
    .min = INT_MAX,
    .max = INT_MIN,
    .nonzero = 0,
    .transitions = 0,
  };
  int prev_sign = 0;
  for (size_t i = 0; i < size; i++) {
    int sample = buffer[i];
    if (sample < stats.min) {
      stats.min = sample;
    }
    if (sample > stats.max) {
      stats.max = sample;
    }
    if (sample != 0) {
      stats.nonzero++;
    }
    int sign = sample > 0 ? 1 : (sample < 0 ? -1 : 0);
    if (prev_sign && sign && sign != prev_sign) {
      stats.transitions++;
    }
    if (sign) {
      prev_sign = sign;
    }
  }
  return stats;
}

static void test_output_format(void) {
  size_t size = 0;
  (void)pb_audio_debug_buffer(&size);
  expect(pb_audio_debug_sample_rate() == 16000, "audio sample rate is not 16 kHz");
  expect(size == 264, "audio pump is not one 16.5 ms 16 kHz block");
}

static void enable_master(uint8_t route_mask) {
  pb_audio_init();
  pb_audio_set_enabled(true);
  expect(pb_audio_enabled(), "desktop audio did not enable");
  audio_write(0xFF26, 0x80);
  audio_write(0xFF24, 0x77);
  audio_write(0xFF25, route_mask);
}

static void expect_waveform(const char *name, int min_max, unsigned min_transitions) {
  pb_audio_pump();
  AudioStats stats = stats_for_buffer();
  if (!(stats.min <= -min_max && stats.max >= min_max &&
        stats.nonzero > 0 && stats.transitions >= min_transitions)) {
    fprintf(stderr, "%s stats min=%d max=%d nonzero=%u transitions=%u\n",
            name, stats.min, stats.max, stats.nonzero, stats.transitions);
    exit(1);
  }
}

static void test_pulse(void) {
  enable_master(0x11);
  audio_write(0xFF11, 0x80);
  audio_write(0xFF12, 0xF0);
  audio_write(0xFF13, 0xD6);
  audio_write(0xFF14, 0x86);
  expect_waveform("pulse", 7000, 8);
}

static void test_wave(void) {
  enable_master(0x44);
  for (uint16_t i = 0; i < 16; i++) {
    audio_write((uint16_t)(0xFF30 + i), (uint8_t)((i << 4) | (15 - i)));
  }
  audio_write(0xFF1A, 0x80);
  audio_write(0xFF1C, 0x20);
  audio_write(0xFF1D, 0xD6);
  audio_write(0xFF1E, 0x86);
  expect_waveform("wave", 2000, 4);
}

static void test_noise(void) {
  enable_master(0x88);
  audio_write(0xFF21, 0xF0);
  audio_write(0xFF22, 0x03);
  audio_write(0xFF23, 0x80);
  expect_waveform("noise", 7000, 8);
}

static void test_master_mute(void) {
  test_pulse();
  audio_write(0xFF26, 0x00);
  pb_audio_pump();
  AudioStats stats = stats_for_buffer();
  expect(stats.nonzero == 0, "master mute left nonzero PCM samples");
}

static void test_silence_pump(void) {
  test_pulse();
  pb_audio_pump_silence();
  AudioStats stats = stats_for_buffer();
  expect(stats.nonzero == 0, "silence pump left nonzero PCM samples");
  expect(pb_audio_stats()->last_write_size > 0, "silence pump did not write PCM");
}

int main(void) {
  test_output_format();
  test_pulse();
  test_wave();
  test_noise();
  test_master_mute();
  test_silence_pump();
  printf("audio mixer test passed\n");
  return 0;
}
