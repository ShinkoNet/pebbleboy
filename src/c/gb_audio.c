#include "gb_audio.h"

#include <string.h>

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define AUDIO_REG_BASE 0xFF10u
#define AUDIO_REG_COUNT 0x30u
#define AUDIO_SAMPLE_RATE 8000u
#define AUDIO_PUMP_SAMPLES 264u
#define AUDIO_PHASE_ONE 65536u
#define AUDIO_WAVE_PHASE_ONE (32u * AUDIO_PHASE_ONE)
#define AUDIO_ENV_TICK_SAMPLES (AUDIO_SAMPLE_RATE / 64u)

typedef struct {
  bool enabled;
  uint32_t phase;
  uint8_t volume;
  uint32_t env_samples;
} PulseChannel;

typedef struct {
  bool enabled;
  uint32_t phase;
} WaveChannel;

typedef struct {
  bool enabled;
  uint32_t phase;
  uint16_t lfsr;
  uint8_t volume;
  uint32_t env_samples;
} NoiseChannel;

static bool s_enabled;
static uint8_t s_regs[AUDIO_REG_COUNT];
static int8_t s_buffer[AUDIO_PUMP_SAMPLES];
static PulseChannel s_pulse1;
static PulseChannel s_pulse2;
static WaveChannel s_wave;
static NoiseChannel s_noise;
static PbAudioStats s_stats;

static uint8_t prv_idx(uint16_t addr) {
  return (uint8_t)(addr - AUDIO_REG_BASE);
}

static bool prv_master_enabled(void) {
  return (s_regs[0x16] & 0x80) != 0;
}

static uint16_t prv_freq_raw(uint8_t lo_idx, uint8_t hi_idx) {
  return (uint16_t)s_regs[lo_idx] | (uint16_t)((s_regs[hi_idx] & 0x07) << 8);
}

static uint32_t prv_pulse_step(uint8_t lo_idx, uint8_t hi_idx) {
  uint16_t raw = prv_freq_raw(lo_idx, hi_idx);
  if (raw >= 2048) {
    return 0;
  }
  uint32_t hz = 131072u / (2048u - raw);
  return (uint32_t)(((uint64_t)hz * AUDIO_PHASE_ONE) / AUDIO_SAMPLE_RATE);
}

static uint32_t prv_wave_step(void) {
  uint16_t raw = prv_freq_raw(0x0D, 0x0E);
  if (raw >= 2048) {
    return 0;
  }
  uint32_t hz = 65536u / (2048u - raw);
  return (uint32_t)(((uint64_t)hz * AUDIO_WAVE_PHASE_ONE) / AUDIO_SAMPLE_RATE);
}

static uint32_t prv_noise_step(void) {
  static const uint16_t divisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};
  uint8_t reg = s_regs[0x12];
  uint32_t divisor = divisors[reg & 0x07];
  uint8_t shift = reg >> 4;
  uint32_t hz = 524288u / divisor;
  if (shift < 15) {
    hz >>= shift;
  } else {
    hz = 0;
  }
  return (uint32_t)(((uint64_t)hz * AUDIO_PHASE_ONE) / AUDIO_SAMPLE_RATE);
}

static void prv_env_tick(uint8_t env_idx, uint8_t *volume, uint32_t *samples) {
  uint8_t period = s_regs[env_idx] & 0x07;
  if (!period) {
    return;
  }
  *samples += 1;
  uint32_t target = (uint32_t)period * AUDIO_ENV_TICK_SAMPLES;
  if (*samples < target) {
    return;
  }
  *samples = 0;
  if (s_regs[env_idx] & 0x08) {
    if (*volume < 15) {
      *volume += 1;
    }
  } else if (*volume > 0) {
    *volume -= 1;
  }
}

static int16_t prv_render_pulse(PulseChannel *ch, uint8_t duty_idx, uint8_t env_idx,
                                uint8_t lo_idx, uint8_t hi_idx) {
  if (!prv_master_enabled() || !ch->enabled || (s_regs[env_idx] & 0xF8) == 0) {
    return 0;
  }

  static const uint16_t thresholds[4] = {
    AUDIO_PHASE_ONE / 8,
    AUDIO_PHASE_ONE / 4,
    AUDIO_PHASE_ONE / 2,
    (AUDIO_PHASE_ONE * 3) / 4
  };
  uint32_t step = prv_pulse_step(lo_idx, hi_idx);
  ch->phase = (ch->phase + step) & (AUDIO_PHASE_ONE - 1u);
  prv_env_tick(env_idx, &ch->volume, &ch->env_samples);
  uint8_t duty = s_regs[duty_idx] >> 6;
  int16_t sample = ch->phase < thresholds[duty] ? 1 : -1;
  return (int16_t)(sample * (int16_t)ch->volume * 8);
}

static int16_t prv_render_wave(void) {
  if (!prv_master_enabled() || !s_wave.enabled || (s_regs[0x0A] & 0x80) == 0) {
    return 0;
  }

  uint8_t level = (s_regs[0x0C] >> 5) & 0x03;
  if (!level) {
    return 0;
  }

  s_wave.phase += prv_wave_step();
  while (s_wave.phase >= AUDIO_WAVE_PHASE_ONE) {
    s_wave.phase -= AUDIO_WAVE_PHASE_ONE;
  }

  uint8_t sample_idx = (uint8_t)((s_wave.phase >> 16) & 0x1F);
  uint8_t packed = s_regs[0x20 + (sample_idx >> 1)];
  uint8_t nibble = (sample_idx & 1) ? (packed & 0x0F) : (packed >> 4);
  int16_t sample = (int16_t)nibble - 8;
  if (level == 2) {
    sample >>= 1;
  } else if (level == 3) {
    sample >>= 2;
  }
  return (int16_t)(sample * 8);
}

static void prv_noise_clock(void) {
  uint16_t bit = (uint16_t)((s_noise.lfsr ^ (s_noise.lfsr >> 1)) & 1u);
  s_noise.lfsr = (uint16_t)((s_noise.lfsr >> 1) | (bit << 14));
  if (s_regs[0x12] & 0x08) {
    s_noise.lfsr = (uint16_t)((s_noise.lfsr & ~(1u << 6)) | (bit << 6));
  }
}

static int16_t prv_render_noise(void) {
  if (!prv_master_enabled() || !s_noise.enabled || (s_regs[0x11] & 0xF8) == 0) {
    return 0;
  }

  s_noise.phase += prv_noise_step();
  while (s_noise.phase >= AUDIO_PHASE_ONE) {
    s_noise.phase -= AUDIO_PHASE_ONE;
    prv_noise_clock();
  }
  prv_env_tick(0x11, &s_noise.volume, &s_noise.env_samples);
  int16_t sample = (s_noise.lfsr & 1) ? -1 : 1;
  return (int16_t)(sample * (int16_t)s_noise.volume * 8);
}

static bool prv_channel_routed(uint8_t channel) {
  uint8_t nr51 = s_regs[0x15];
  uint8_t mask = (uint8_t)((1u << channel) | (1u << (channel + 4)));
  return (nr51 & mask) != 0;
}

static int8_t prv_mix_sample(void) {
  int16_t mix = 0;
  if (prv_channel_routed(0)) {
    mix += prv_render_pulse(&s_pulse1, 0x01, 0x02, 0x03, 0x04);
  }
  if (prv_channel_routed(1)) {
    mix += prv_render_pulse(&s_pulse2, 0x06, 0x07, 0x08, 0x09);
  }
  if (prv_channel_routed(2)) {
    mix += prv_render_wave();
  }
  if (prv_channel_routed(3)) {
    mix += prv_render_noise();
  }

  uint8_t nr50 = s_regs[0x14];
  uint8_t volume = (uint8_t)(((nr50 & 0x07) + ((nr50 >> 4) & 0x07) + 2) / 2);
  mix = (int16_t)((mix * volume) / 7);
  if (mix > 127) {
    return 127;
  }
  if (mix < -128) {
    return -128;
  }
  return (int8_t)mix;
}

static void prv_trigger_pulse(PulseChannel *ch, uint8_t env_idx) {
  ch->enabled = (s_regs[env_idx] & 0xF8) != 0;
  ch->phase = 0;
  ch->volume = s_regs[env_idx] >> 4;
  ch->env_samples = 0;
}

static void prv_trigger_wave(void) {
  s_wave.enabled = (s_regs[0x0A] & 0x80) != 0;
  s_wave.phase = 0;
}

static void prv_trigger_noise(void) {
  s_noise.enabled = (s_regs[0x11] & 0xF8) != 0;
  s_noise.phase = 0;
  s_noise.lfsr = 0x7FFF;
  s_noise.volume = s_regs[0x11] >> 4;
  s_noise.env_samples = 0;
}

void pb_audio_init(void) {
  memset(s_regs, 0, sizeof(s_regs));
  memset(&s_pulse1, 0, sizeof(s_pulse1));
  memset(&s_pulse2, 0, sizeof(s_pulse2));
  memset(&s_wave, 0, sizeof(s_wave));
  memset(&s_noise, 0, sizeof(s_noise));
  memset(&s_stats, 0, sizeof(s_stats));
  s_enabled = false;
}

void pb_audio_set_enabled(bool enabled) {
#ifndef PB_DESKTOP
  if (enabled && !s_enabled) {
    s_enabled = speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 50);
    if (!s_enabled) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "speaker stream unavailable");
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "speaker stream enabled");
    }
  } else if (!enabled && s_enabled) {
    speaker_stream_close();
    s_enabled = false;
    APP_LOG(APP_LOG_LEVEL_INFO, "speaker stream disabled");
  }
#else
  s_enabled = enabled;
#endif
}

void pb_audio_pump(void) {
  if (!s_enabled) {
    return;
  }

  for (uint16_t i = 0; i < AUDIO_PUMP_SAMPLES; i++) {
    s_buffer[i] = prv_mix_sample();
  }
  s_stats.pumps++;

#ifndef PB_DESKTOP
  uint32_t written = speaker_stream_write(s_buffer, sizeof(s_buffer));
  s_stats.last_write_size = written;
  if (written < sizeof(s_buffer)) {
    s_stats.partial_writes++;
    if ((s_stats.partial_writes & 0x1F) == 1) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "speaker partial write %lu/%u partials=%lu",
              written, (unsigned)sizeof(s_buffer),
              (unsigned long)s_stats.partial_writes);
    }
  }
  if ((s_stats.pumps & 0x3Fu) == 1) {
    APP_LOG(APP_LOG_LEVEL_INFO, "audio pumps=%lu partial=%lu last_write=%lu",
            s_stats.pumps, s_stats.partial_writes, s_stats.last_write_size);
  }
#else
  s_stats.last_write_size = sizeof(s_buffer);
#endif
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

const PbAudioStats *pb_audio_stats(void) {
  return &s_stats;
}

#ifdef PB_DESKTOP
const int8_t *pb_audio_debug_buffer(size_t *size_out) {
  if (size_out) {
    *size_out = sizeof(s_buffer);
  }
  return s_buffer;
}
#endif

uint8_t audio_read(uint16_t addr) {
  if (addr < AUDIO_REG_BASE || addr >= AUDIO_REG_BASE + AUDIO_REG_COUNT) {
    return 0xFF;
  }
  static const uint8_t ortab[AUDIO_REG_COUNT] = {
    0x80, 0x3f, 0x00, 0xff, 0xbf,
    0xff, 0x3f, 0x00, 0xff, 0xbf,
    0x7f, 0xff, 0x9f, 0xff, 0xbf,
    0xff, 0xff, 0x00, 0x00, 0xbf,
    0x00, 0x00, 0x70,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  uint8_t idx = prv_idx(addr);
  return s_regs[idx] | ortab[idx];
}

void audio_write(uint16_t addr, uint8_t val) {
  if (addr < AUDIO_REG_BASE || addr >= AUDIO_REG_BASE + AUDIO_REG_COUNT) {
    return;
  }

  uint8_t idx = prv_idx(addr);
  if (idx == 0x16) {
    s_regs[idx] = val & 0x80;
    if ((val & 0x80) == 0) {
      s_pulse1.enabled = false;
      s_pulse2.enabled = false;
      s_wave.enabled = false;
      s_noise.enabled = false;
    }
    return;
  }

  s_regs[idx] = val;
  if (!prv_master_enabled() && idx < 0x20) {
    return;
  }

  switch (idx) {
    case 0x04:
      if (val & 0x80) {
        prv_trigger_pulse(&s_pulse1, 0x02);
      }
      break;
    case 0x09:
      if (val & 0x80) {
        prv_trigger_pulse(&s_pulse2, 0x07);
      }
      break;
    case 0x0E:
      if (val & 0x80) {
        prv_trigger_wave();
      }
      break;
    case 0x13:
      if (val & 0x80) {
        prv_trigger_noise();
      }
      break;
    default:
      break;
  }
}
