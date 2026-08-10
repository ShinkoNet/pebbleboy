# Pebbleboy Plan

Pebbleboy is a DMG-only Game Boy emulator for Pebble Time 2 / Emery. The
implementation targets the modern Core Devices Pebble SDK, QEMU, and real
hardware, with audio through the speaker PCM stream and input through the four
buttons plus the touchscreen.

The initial target ROM is `tetris.gb` at 32 KiB. Larger ROMs such as
`pokered.gb` at 1 MiB are supported through banked resource access or a
phone-backed ROM bank service. ROMs are for local testing only and are never
distributed with the app.

## Goals

- Emulate original Game Boy / DMG only.
- Run small 32 KiB ROMs first, then MBC-backed ROMs.
- Support speaker audio through a software mixer and Pebble PCM streaming.
- Use the touchscreen as a virtual D-pad.
- Keep runtime memory comfortably inside the 128 KiB app code + heap class.
- Use Pebble resources and phone-side storage as backing stores rather than
  loading full cartridges into heap.
- Build a testable core with desktop, QEMU, and hardware validation.

## Non-Goals

- No Game Boy Color support.
- No Super Game Boy support.
- No link cable.
- No save states in the first version.
- No rewind, debugger, cheats, or fast-forward UI in the first version.
- No bundled copyrighted ROMs.

## Platform Constraints

Pebble Time 2 / Emery gives us a strong CPU for this class of emulator, but
the app still needs to be deliberate about memory:

- CPU: 240 MHz Cortex-M33-like Star-MC1.
- Display: 200 x 228, 64 colors.
- Game Boy display: 160 x 144, 4 shades.
- App class: 128 KiB code + heap.
- App-store resources: 256 KiB class; sideloaded/dev resources may be larger.
- Speaker API supports mono signed PCM at 8/16 kHz and 8/16-bit.
- Touch API provides single-point touch events.
- E-Paper Refresh rate is 30 frames per second, instead of the gameboy's 59.727 Hz.

The display fits cleanly at 1:1 scale: center the 160 x 144 Game Boy viewport
at x=20, y=42. However, we should have a scaling mode to toggle between 1:1 and 
fullscreen (200 x 228) along with scaling method options in the config page.

## Emulator Core
 
Start with Peanut-GB as the core candidate:

- C99, portable, compact.
- DMG support.
- MBC1, MBC2, MBC3, MBC5 support.
- Line-based LCD callback.
- Compile-time options for LCD and sound.

Keep the core behind a small platform interface:

```c
uint8_t cart_read(uint16_t addr);
void cart_write(uint16_t addr, uint8_t value);
void video_draw_line(uint8_t y, const uint8_t *shade_line);
void input_set_buttons(uint8_t buttons);
void audio_step(uint32_t cycles);
```

This avoids spreading Pebble-specific resource, AppMessage, and speaker code
through the emulator.

## ROM Backing

Use three ROM modes with the same cartridge cache interface.

### 32 KiB Local ROM Mode

For `tetris.gb`, load the two 16 KiB ROM banks from a raw Pebble resource:

- Bank 0: fixed `0000-3FFF`.
- Bank 1: switchable window `4000-7FFF`, though no MBC is needed.

This is the simplest first boot path.

### Resource-Backed Bank Mode

For larger ROMs available as Pebble resources, keep the cartridge in flash and
load cache banks with `resource_load_byte_range()`.

The logical cache unit is 16 KiB because this matches the Game Boy cartridge
bank window. 32 KiB physical reads can be added later as a prefetch
optimization, but the emulator should reason in 16 KiB banks.

### Phone-Backed Bank Mode

For ROMs too large or unsuitable for resources, the phone-side JavaScript
fetches a user-provided ROM URL from settings and caches the ROM in phone
storage. The watch requests 16 KiB banks on demand.

Protocol shape:

```text
Watch -> Phone: ROM_INFO_REQUEST
Phone -> Watch: ROM_INFO {size, sha1, title, cart_type}

Watch -> Phone: ROM_BANK_REQUEST {bank, offset, size}
Phone -> Watch: ROM_BANK_BEGIN {bank, offset, size}
Phone -> Watch: ROM_BANK_DATA {bank, offset, bytes}
Phone -> Watch: ROM_BANK_END {bank, offset, size, sha1}
```

The emulator pauses on a cache miss, shows a small loading indicator if needed,
then resumes when the requested bank is complete. Failed bank loads should
leave the emulator paused with an actionable error.

## Bank Cache

Start with three resident ROM banks:

```text
16K fixed bank 0
16K active switch bank
16K LRU or prefetch switch bank
```

This gives a good balance between performance and heap margin. A fourth bank is
allowed after profiling:

```text
16K fixed bank 0
16K active switch bank
16K previous switch bank
16K prefetch switch bank
```

Four banks are attractive for large games, but only if audio, cart RAM, stack,
and Pebble services still leave comfortable headroom.

Cache policy:

- Bank 0 is pinned.
- The currently selected MBC bank is pinned while active.
- Remaining slots are LRU.
- The cache is subdivided into line slots for phone-backed ROMs. Fixed bank 0
  can still be filled as one 16 KiB request, while switched banks use
  AppMessage-sized demand fills so the emulator resumes after a short line
  transfer instead of waiting for a whole bank.
- After a demand fill, optionally prefetch one adjacent line if a free or
  low-value slot exists. Do not chain prefetch completions into whole-bank
  streaming, because demand misses must stay ahead of background lookahead.
- MBC writes trigger cache selection and possible async loading.

## Memory Budget

Target a design that does not depend on using every byte.

Conservative first large-ROM budget:

```text
16K fixed ROM bank 0
16K active switch ROM bank
16K LRU/prefetch ROM bank
32K cartridge RAM for battery-backed games
8K  Game Boy WRAM
8K  Game Boy VRAM
6K  packed 2bpp screen buffer
2K  audio ring buffer
~5K OAM, HRAM, CPU, PPU, APU, MBC, UI, AppMessage state
```

This lands near 104 KiB before stack and runtime overhead. The fourth ROM bank
is a profiling-driven optimization, not a baseline requirement.

For games without battery RAM, reuse that space for a fourth ROM bank or a
larger audio buffer.

## Video

Use a packed 2bpp framebuffer:

- Game Boy has four shades, so 2 bits per pixel is sufficient.
- 160 x 144 x 2bpp = 5,760 bytes.
- Expand to Pebble framebuffer colors during the layer update proc.

Initial palette:

```text
0 white / lightest
1 light gray-green
2 dark gray-green
3 black / darkest
```

Later palettes can be simple four-entry lookup tables. Do not store palette
images as resources.

## Input

Button mapping:

```text
Select button -> A
Down button   -> B
Up button     -> Start
Back click    -> Select
Back hold     -> system quit behavior
```

Touchscreen D-pad:

- Treat the touch area as a square centered on the display.
- Split it by diagonals into four directional quadrants.
- Track touchdown, position update, and liftoff.
- Sliding between quadrants releases the old direction and presses the new one.

Classification:

```c
dx = x - center_x;
dy = y - center_y;
if (abs(dx) > abs(dy)) {
  direction = dx < 0 ? LEFT : RIGHT;
} else {
  direction = dy < 0 ? UP : DOWN;
}
```

Physical button input should clear any stuck touch direction.

## Audio

Use `speaker_stream_open()` with app-generated PCM.

Current format:

```text
SpeakerPcmFormat_16kHz_16bit
mono signed PCM, generated once per emulated frame
```

Mixer:

- Channel 1: pulse with sweep.
- Channel 2: pulse.
- Channel 3: wave channel.
- Channel 4: noise channel with LFSR.
- Mix to mono.
- Use fixed-point phase accumulators.
- Avoid float in the audio path.

The mixer should run from emulated cycles so audio stays synchronized with the
CPU/APU state. Track underruns and partial `speaker_stream_write()` returns in
logs.

## Save RAM

For battery-backed games:

- Keep cartridge RAM in watch heap while running.
- Persist small cart RAM locally only when feasible.
- For 32 KiB saves such as Pokemon Red, sync to phone storage.
- Use explicit save flush points: app exit, periodic timer, and after SRAM
  bank writes settle.

Phone sync protocol:

```text
Watch -> Phone: SRAM_SAVE {cart_sha1, size, chunks...}
Watch -> Phone: SRAM_LOAD_REQUEST {cart_sha1}
Phone -> Watch: SRAM_LOAD_DATA {offset, bytes}
```

Save files are keyed by ROM hash, not only title.

## Testing

Use test ROMs before commercial games:

- Blargg CPU instruction tests.
- Blargg instruction timing tests.
- Blargg DMG sound tests.
- Mooneye DMG acceptance subset.
- Small legal homebrew ROMs.
- `tetris.gb` as the first local gameplay target.
- `pokered.gb` as the first large MBC + SRAM stress target.

Test layers:

1. Desktop harness with framebuffer hashes.
2. Pebble QEMU screenshots.
3. Longer QEMU Pokemon title-screen screenshot checks for large-ROM visual
   regressions such as black sprite boxes.
4. QEMU audio capture for basic waveform checks.
5. Real Pebble Time 2 performance, audio, and input testing.

Log at runtime:

- FPS and frame time.
- Heap free and heap used.
- ROM cache hits and misses.
- Bank load latency.
- Audio underruns.
- AppMessage retries/failures.

## Milestones

1. Scaffold Pebble app in `Pebbleboy`.
2. Display centered 160 x 144 test pattern on Emery.
3. Build emulator core with LCD enabled and audio disabled.
4. Load and boot `tetris.gb` from local resource.
5. Render frames through the packed 2bpp framebuffer.
6. Add button and touch input.
7. Add frame pacing and basic performance logs.
8. Add MBC abstraction and 16 KiB ROM bank cache.
9. Support resource-backed bank loading with `resource_load_byte_range()`.
10. Add phone-backed ROM URL fetch and 16 KiB bank service.
11. Boot `pokered.gb` to title screen without SRAM.
12. Add cartridge RAM and phone-backed save sync.
13. Add PCM APU mixer.
14. Profile 3-bank vs 4-bank cache on QEMU and hardware.
15. Polish loading/error UI and configuration.

## Build Layout

Proposed repo structure:

```text
Pebbleboy/
  PLAN.md
  package.json
  wscript
  src/
    c/
      main.c
      gb_app.c
      gb_cart.c
      gb_cart.h
      gb_video.c
      gb_video.h
      gb_audio.c
      gb_audio.h
      gb_input.c
      gb_input.h
      peanut_gb.h
    pkjs/
      index.js
  resources/
    data/
      tetris.gb       local test only, gitignored
  roms/
    tetris.gb         local test only, gitignored
    pokered.gb        local test only, gitignored
  tools/
    sync_roms.py
    run_tests.sh
```

ROM files should stay gitignored. Test hashes and metadata can be committed.

## Implementation Notes

- Keep the emulator loop cooperative with Pebble's event model.
- Avoid large automatic stack buffers.
- Prefer static or heap-owned buffers with explicit size accounting.
- Use `heap_bytes_free()` and `heap_bytes_used()` logs on Emery.
- Keep AppMessage chunk sizes conservative and retry failed chunks.
- Make every ROM source produce the same `cart_read` behavior so local,
  resource-backed, and phone-backed modes share the emulator core.
