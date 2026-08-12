#include "gb_audio.h"

#include <string.h>

#ifndef PB_DESKTOP
#include <pebble.h>
#endif

#define AUDIO_REG_BASE 0xFF10u
#define AUDIO_REG_COUNT 0x30u
#define AUDIO_SAMPLE_RATE 16000u
#define AUDIO_PUMP_SAMPLES PB_AUDIO_PUMP_SAMPLES
#define AUDIO_PHASE_ONE 65536u
#define AUDIO_WAVE_PHASE_ONE (32u * AUDIO_PHASE_ONE)
#define AUDIO_ENV_TICK_SAMPLES (AUDIO_SAMPLE_RATE / 64u)
#define AUDIO_MIX_SCALE 4
#define AUDIO_DC_BLOCK_Q15 32604

typedef struct {
  bool enabled;
  uint32_t phase;
  uint32_t step;
  uint8_t volume;
  uint32_t env_samples;
} PulseChannel;

typedef struct {
  bool enabled;
  uint32_t phase;
  uint32_t step;
} WaveChannel;

typedef struct {
  bool enabled;
  uint32_t phase;
  uint32_t step;
  uint16_t lfsr;
  uint8_t volume;
  uint32_t env_samples;
} NoiseChannel;

static bool s_requested;
static bool s_enabled;
static uint8_t s_regs[AUDIO_REG_COUNT];
static int16_t s_buffer[AUDIO_PUMP_SAMPLES];
static PulseChannel s_pulse1;
static PulseChannel s_pulse2;
static WaveChannel s_wave;
static NoiseChannel s_noise;
static PbAudioStats s_stats;
static uint16_t s_pending_offset;
static uint16_t s_pending_size;
static bool s_master_enabled;
static uint8_t s_route_mask;
static uint8_t s_channel_gain[4];
static int32_t s_dc_prev_input;
static int32_t s_dc_prev_output;

static uint8_t prv_idx(uint16_t addr) {
  return (uint8_t)(addr - AUDIO_REG_BASE);
}

static uint16_t prv_freq_raw(uint8_t lo_idx, uint8_t hi_idx) {
  return (uint16_t)s_regs[lo_idx] | (uint16_t)((s_regs[hi_idx] & 0x07) << 8);
}

static uint32_t prv_pulse_step(uint8_t lo_idx, uint8_t hi_idx) {
  uint16_t raw = prv_freq_raw(lo_idx, hi_idx);
  if (raw >= 2048) {
    return 0;
  }
  return (uint32_t)(((uint64_t)131072u * AUDIO_PHASE_ONE) /
                    ((uint32_t)(2048u - raw) * AUDIO_SAMPLE_RATE));
}

static uint32_t prv_wave_step(void) {
  uint16_t raw = prv_freq_raw(0x0D, 0x0E);
  if (raw >= 2048) {
    return 0;
  }
  return (uint32_t)(((uint64_t)65536u * AUDIO_WAVE_PHASE_ONE) /
                    ((uint32_t)(2048u - raw) * AUDIO_SAMPLE_RATE));
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

static int16_t prv_render_pulse(PulseChannel *ch, uint8_t duty_idx, uint8_t env_idx) {
  if (!ch->enabled || (s_regs[env_idx] & 0xF8) == 0) {
    return 0;
  }

  static const uint16_t thresholds[4] = {
    AUDIO_PHASE_ONE / 8,
    AUDIO_PHASE_ONE / 4,
    AUDIO_PHASE_ONE / 2,
    (AUDIO_PHASE_ONE * 3) / 4
  };
  ch->phase = (ch->phase + ch->step) & (AUDIO_PHASE_ONE - 1u);
  prv_env_tick(env_idx, &ch->volume, &ch->env_samples);
  uint8_t duty = s_regs[duty_idx] >> 6;
  int16_t sample = ch->phase < thresholds[duty] ? 1 : -1;
  return (int16_t)(sample * (int16_t)ch->volume * 8);
}

static int16_t prv_render_wave(void) {
  if (!s_wave.enabled || (s_regs[0x0A] & 0x80) == 0) {
    return 0;
  }

  uint8_t level = (s_regs[0x0C] >> 5) & 0x03;
  if (!level) {
    return 0;
  }

  s_wave.phase += s_wave.step;
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
  if (!s_noise.enabled || (s_regs[0x11] & 0xF8) == 0) {
    return 0;
  }

  s_noise.phase += s_noise.step;
  while (s_noise.phase >= AUDIO_PHASE_ONE) {
    s_noise.phase -= AUDIO_PHASE_ONE;
    prv_noise_clock();
  }
  prv_env_tick(0x11, &s_noise.volume, &s_noise.env_samples);
  int16_t sample = (s_noise.lfsr & 1) ? -1 : 1;
  return (int16_t)(sample * (int16_t)s_noise.volume * 8);
}

static int16_t prv_mix_sample(void) {
  if (!s_master_enabled) {
    return 0;
  }

  // Advance every enabled channel even while it is temporarily unrouted. This
  // keeps phase and envelope state continuous when NR51 changes. Fold the two
  // GB output terminals into mono with their independent NR50 gains.
  int32_t mix = 0;
  mix += (int32_t)prv_render_pulse(&s_pulse1, 0x01, 0x02) * s_channel_gain[0];
  mix += (int32_t)prv_render_pulse(&s_pulse2, 0x06, 0x07) * s_channel_gain[1];
  mix += (int32_t)prv_render_wave() * s_channel_gain[2];
  mix += (int32_t)prv_render_noise() * s_channel_gain[3];
  mix *= AUDIO_MIX_SCALE;

  // The DMG output is AC-coupled. A cheap fixed-point DC blocker removes the
  // large offsets from 12.5/25/75% pulse duty cycles without blurring edges.
  int32_t filtered = mix - s_dc_prev_input +
                     ((s_dc_prev_output * AUDIO_DC_BLOCK_Q15) >> 15);
  s_dc_prev_input = mix;
  if (filtered > INT16_MAX) {
    filtered = INT16_MAX;
  } else if (filtered < INT16_MIN) {
    filtered = INT16_MIN;
  }
  s_dc_prev_output = filtered;
  return (int16_t)filtered;
}

static void prv_update_mix_gains(void) {
  uint8_t nr50 = s_regs[0x14];
  uint8_t nr51 = s_regs[0x15];
  uint8_t right_gain = (uint8_t)((nr50 & 0x07) + 1);
  uint8_t left_gain = (uint8_t)(((nr50 >> 4) & 0x07) + 1);
  s_route_mask = (uint8_t)((nr51 | (nr51 >> 4)) & 0x0F);
  for (uint8_t channel = 0; channel < 4; channel++) {
    s_channel_gain[channel] =
        (uint8_t)(((nr51 & (1u << channel)) ? right_gain : 0) +
                  ((nr51 & (1u << (channel + 4))) ? left_gain : 0));
  }
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
  s_pending_offset = 0;
  s_pending_size = 0;
  s_master_enabled = false;
  s_route_mask = 0;
  memset(s_channel_gain, 0, sizeof(s_channel_gain));
  s_dc_prev_input = 0;
  s_dc_prev_output = 0;
  s_requested = false;
  s_enabled = false;
}

#if !defined(PB_DESKTOP) && !defined(PEBBLEBOY_NO_AUDIO)
static bool prv_open_stream(void) {
  if (s_enabled) {
    return true;
  }
  s_enabled = speaker_stream_open(SpeakerPcmFormat_16kHz_16bit, 50);
  if (!s_enabled) {
    s_stats.stream_errors++;
    if ((s_stats.stream_errors & 0x3Fu) == 1) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "speaker stream unavailable errors=%lu",
              s_stats.stream_errors);
    }
    return false;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "speaker stream enabled 16kHz 16-bit");
  return true;
}

static void prv_close_stream(const char *reason) {
  if (s_enabled) {
    speaker_stream_close();
    s_enabled = false;
  }
  if (reason) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "%s", reason);
  }
  s_pending_offset = 0;
  s_pending_size = 0;
}
#endif

void pb_audio_set_enabled(bool enabled) {
#ifdef PEBBLEBOY_NO_AUDIO
  (void)enabled;
  s_requested = false;
#else
  s_requested = enabled;
#ifndef PB_DESKTOP
  if (!enabled) {
    prv_close_stream("speaker stream disabled");
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "speaker stream armed");
  }
#else
  s_enabled = enabled;
#endif
#endif
}

void pb_audio_suspend_stream(void) {
#if defined(PEBBLEBOY_NO_AUDIO)
  s_enabled = false;
#elif !defined(PB_DESKTOP)
  prv_close_stream(NULL);
#else
  (void)s_enabled;
#endif
}

static void prv_write_buffer(void) {
  s_stats.pumps++;

  uint16_t remaining = (uint16_t)(s_pending_size - s_pending_offset);
  if (!remaining) {
    return;
  }

#if !defined(PB_DESKTOP) && !defined(PEBBLEBOY_NO_AUDIO)
  uint32_t written = speaker_stream_write(
      (const uint8_t *)s_buffer + s_pending_offset, remaining);
#else
  uint32_t written = remaining;
#endif
  s_stats.last_write_size = written;
  s_pending_offset = (uint16_t)(s_pending_offset + written);
  if (written < remaining) {
    s_stats.partial_writes++;
#if !defined(PB_DESKTOP) && !defined(PEBBLEBOY_NO_AUDIO)
    if ((s_stats.partial_writes & 0xFFu) == 1) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "speaker partial write %lu/%u partials=%lu",
              written, (unsigned)remaining,
              (unsigned long)s_stats.partial_writes);
    }
#endif
  }
#if !defined(PB_DESKTOP) && !defined(PEBBLEBOY_NO_AUDIO)
  if ((s_stats.pumps & 0x1FFu) == 1) {
    APP_LOG(APP_LOG_LEVEL_INFO, "audio pumps=%lu partial=%lu errors=%lu last_write=%lu",
            s_stats.pumps, s_stats.partial_writes, s_stats.stream_errors,
            s_stats.last_write_size);
  }
#endif
  if (s_pending_offset >= s_pending_size) {
    s_pending_offset = 0;
    s_pending_size = 0;
  }
}

static bool prv_prepare_stream(void) {
#ifdef PEBBLEBOY_NO_AUDIO
  return false;
#else
  if (!s_requested) {
    return false;
  }

#ifndef PB_DESKTOP
  if (!prv_open_stream()) {
    return false;
  }
#endif
  return true;
#endif
}

static void prv_record_mix_stats(uint16_t nonzero, uint16_t peak) {
  s_stats.generated_buffers++;
  s_stats.last_nonzero_samples = nonzero;
  s_stats.last_peak = peak;
  s_stats.active_channels = (s_pulse1.enabled ? 1u : 0u) |
                            (s_pulse2.enabled ? 2u : 0u) |
                            (s_wave.enabled ? 4u : 0u) |
                            (s_noise.enabled ? 8u : 0u);
  s_stats.nr50 = s_regs[0x14];
  s_stats.nr51 = s_regs[0x15];
  s_stats.nr52 = s_regs[0x16];
}

void pb_audio_pump(void) {
  if (!prv_prepare_stream()) {
    return;
  }

  if (s_pending_size) {
    prv_write_buffer();
    return;
  }

  uint16_t nonzero = 0;
  uint16_t peak = 0;
  for (uint16_t i = 0; i < AUDIO_PUMP_SAMPLES; i++) {
    int16_t sample = prv_mix_sample();
    s_buffer[i] = sample;
    int32_t magnitude = sample;
    if (magnitude < 0) {
      magnitude = -magnitude;
    }
    nonzero += magnitude != 0;
    if (magnitude > peak) {
      peak = (uint16_t)magnitude;
    }
  }
  prv_record_mix_stats(nonzero, peak);
  s_pending_size = sizeof(s_buffer);
  prv_write_buffer();
}

void pb_audio_pump_silence(void) {
  if (!prv_prepare_stream()) {
    return;
  }

  if (s_pending_size) {
    prv_write_buffer();
    return;
  }

  memset(s_buffer, 0, sizeof(s_buffer));
  prv_record_mix_stats(0, 0);
  s_pending_size = sizeof(s_buffer);
  prv_write_buffer();
}

void pb_audio_deinit(void) {
  s_requested = false;
#if !defined(PB_DESKTOP) && !defined(PEBBLEBOY_NO_AUDIO)
  prv_close_stream(NULL);
#endif
  s_enabled = false;
}

bool pb_audio_enabled(void) {
  return s_requested;
}

const PbAudioStats *pb_audio_stats(void) {
  return &s_stats;
}

#ifdef PB_DESKTOP
const int16_t *pb_audio_debug_buffer(size_t *sample_count_out) {
  if (sample_count_out) {
    *sample_count_out = AUDIO_PUMP_SAMPLES;
  }
  return s_buffer;
}

uint32_t pb_audio_debug_sample_rate(void) {
  return AUDIO_SAMPLE_RATE;
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
    s_master_enabled = (val & 0x80) != 0;
    if (!s_master_enabled) {
      s_pulse1.enabled = false;
      s_pulse2.enabled = false;
      s_wave.enabled = false;
      s_noise.enabled = false;
      s_dc_prev_input = 0;
      s_dc_prev_output = 0;
    }
    return;
  }

  s_regs[idx] = val;
  switch (idx) {
    case 0x14:
      prv_update_mix_gains();
      break;
    case 0x15:
      prv_update_mix_gains();
      break;
    case 0x03:
    case 0x04:
      s_pulse1.step = prv_pulse_step(0x03, 0x04);
      break;
    case 0x08:
    case 0x09:
      s_pulse2.step = prv_pulse_step(0x08, 0x09);
      break;
    case 0x0D:
    case 0x0E:
      s_wave.step = prv_wave_step();
      break;
    case 0x12:
      s_noise.step = prv_noise_step();
      break;
    default:
      break;
  }
  if (!s_master_enabled && idx < 0x20) {
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
