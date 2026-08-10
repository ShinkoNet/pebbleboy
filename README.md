# Pebbleboy

Pebbleboy is a DMG Game Boy emulator for Pebble Time 2 (Emery/Obelix). It uses
Peanut-GB for the CPU and LCD core, a native 16 kHz/16-bit PCM mixer, and a
phone-backed cartridge cache for ROMs up to 4 MB.

Pebbleboy does not contain or distribute commercial Game Boy ROMs. You must
provide ROM images that you are legally entitled to use.

## Install and configure

The normal `Pebbleboy.pbw` is ROM-free and can be shared without rebuilding it.

1. Install the PBW and keep the watch connected to its companion phone.
2. Open Pebbleboy's settings in the Pebble mobile app.
3. Add one or more game names and direct-download ROM URLs.
4. Choose display scaling and whether speaker audio is enabled, then save.
5. Launch Pebbleboy and choose a game with **Up/Down** and **Select**.

Each URL must return either:

- a binary `.gb` file;
- plain base64; or
- base64 preceded by a line containing `PEBBLEBOY_ROM_BASE64`.

The host must allow the Pebble phone JavaScript to fetch the URL. HTTPS and an
`Access-Control-Allow-Origin: *` response header are recommended. Avoid putting
long-lived secrets in a URL: configuration and cache data are phone-local but
are not an encrypted credential store.

The settings editor is hosted at `https://ptv.netcavy.net/gb/`. Existing ROM
names and URLs are passed to it in the URL fragment, which browsers do not send
to the web server. The static source for the page is in `config/index.html`.

The library supports up to 12 entries. Only the active ROM is kept in phone
memory/cache; save RAM is stored separately under the ROM's SHA-1 identity, so
switching games does not mix their saves.

## Where large ROMs live

The full 1–4 MB cartridge is not copied into Pebble app flash. The phone fetches
the selected ROM and the watch requests 512-byte cartridge lines on demand,
keeping a 48 KB working cache in app RAM. This keeps the PBW below the normal
resource limit and avoids a different build for every user.

The phone must therefore be reachable when the app first loads a game and on a
cache miss. If the phone cannot persist a very large ROM within its JavaScript
storage quota, it may download that ROM again after the phone service restarts.

Copying arbitrary multi-megabyte ROM files permanently onto the watch would
need a separate app-accessible filesystem API or a per-ROM PBW. Pebbleboy does
not require either approach.

## Firmware and audio

ROM selection and large-ROM streaming use ordinary AppMessage APIs; the custom
firmware does not store multi-megabyte cartridges on the watch. Pebbleboy does,
however, use a larger Obelix foreground-app RAM allocation for its 48 KB cache
and emulator state. Its CPU load also exposes speaker refill starvation in the
stock scheduler.

Prebuilt DVT and PVT firmware containing the extra app memory and speaker
scheduling/DMA fixes is published from the
[ShinkoNet PebbleOS fork](https://github.com/ShinkoNet/PebbleOS/releases). The
[firmware notes](https://github.com/ShinkoNet/PebbleOS/blob/main/PEBBLEBOY_FIRMWARE.md)
explain hardware selection, sideload precautions, source patches, and the
automatic upstream-sync process. Each release contains a merged PBZ with both
firmware slots; users do not need to compile PebbleOS themselves.

The current Time 2 speaker API accepts Pebbleboy's mono signed 16 kHz/16-bit
PCM format. Disabling audio reduces load, but does not replace the additional
app-memory allocation required by the current high-performance cache.

## Controls

During a game:

| Pebble input | Game Boy input |
| --- | --- |
| Select button | A |
| Down button | B |
| Up button | Start |
| Back click | Select |
| Touchscreen direction | D-pad |

## Build

With the Pebble SDK active:

```sh
pebble build
```

That produces a ROM-free `build/Pebbleboy.pbw`. For development only, a ROM can
still be embedded at `resources/data/cartridge.gb`:

```sh
PEBBLEBOY_EMBED_ROM=1 pebble build
```

Embedded ROMs must never be committed or distributed unless their licence
explicitly permits it.

## Tests

The desktop suite covers the cartridge cache, phone protocol helpers, SRAM,
audio mixer, Tetris, Pokémon Red when locally available, and public CPU/LCD
compatibility ROMs:

```sh
bash tools/run_tests.sh
bash tools/run_compat_tests.sh
```

Test ROMs are local, gitignored inputs. They are not part of the PBW.

## Credits

The emulator core is based on Peanut-GB by Mahyar Koshkouei and contains
credited MIT-licensed portions from SameBoy. See `src/c/peanut_gb.h` for its
licence and notices.

## Licence

Pebbleboy is distributed under the [MIT License](LICENSE). Third-party
copyright and licence information is collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and retained in the relevant
source files.
