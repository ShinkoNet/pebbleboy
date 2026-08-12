# Pebbleboy

Pebbleboy is a Game Boy and Game Boy Color emulator for Pebble Time 2 and
Pebble 2 Duo. They are the only Pebble models with the 128 KiB app limit needed
by the emulator. It uses Peanut-GB for the CPU and LCD core and can use a native
16 kHz/16-bit PCM mixer on speaker-equipped watches.

Pebbleboy does not contain or distribute commercial Game Boy ROMs. You must
provide ROM images that you are legally entitled to use.

## Release types

Pebbleboy is being developed around two complementary release types:

| Release | ROM delivery | Phone needed while playing | Firmware | Audio |
| --- | --- | --- | --- | --- |
| Universal CFW build | Configure a URL; the phone downloads the selected ROM once and installs it into watch flash | No, after installation | ShinkoNet CFW | Optional |
| Per-ROM stock build | A Linux build script embeds a ROM supplied from the user's own filesystem into a personal PBW | No | Stock-compatible target | Omitted by default |

The universal build needs CFW because its app-scoped 8 MB ROM store is a new
firmware API. The emulator and cartridge cache fit the standard 128 KiB app
limit; the stock target can use an immutable PBW resource and avoid the blob
API entirely. That RAM requirement limits both release types to Pebble Time 2
and Pebble 2 Duo.

Audio is not a stock-release requirement. Only Pebble Time 2 and Pebble 2 Duo
have speakers, while the latter has a monochrome display and is not a Game Boy
Color target. Speaker and scheduler improvements can remain a CFW feature
unless equivalent support lands upstream.

The per-ROM stock builder and polished release artifacts are still planned;
the current development build should be treated as the CFW target.

## Install and configure

The normal `Pebbleboy.pbw` is ROM-free and can be shared without rebuilding it.

1. Install the CFW PBW and connect the watch to its companion phone.
2. Open Pebbleboy's settings in the Pebble mobile app.
3. Add one or more game names and direct-download ROM URLs.
4. Choose display scaling and whether speaker audio is enabled, then save.
5. Launch Pebbleboy and choose a game with **Up/Down** and **Select**. The phone
   transfers it once; subsequent launches read it directly from watch flash.

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

The library supports up to 12 entries. One selected ROM is installed on the
watch at a time. The phone may also cache the active download. Save RAM belongs
to the locally installed cartridge, so gameplay and saving do not depend on a
Bluetooth connection.

## Where large ROMs live

In the universal CFW build, the phone fetches the selected 32 KB–8 MB cartridge
and transfers it in checked AppMessage chunks. CFW stores it in a filesystem
blob owned by Pebbleboy's UUID. The blob is committed only after its size and
CRC32 verify, so an interrupted transfer is never mistaken for a usable ROM.
The emulator then reads cartridge banks locally from flash through its RAM
cache. Installing another game replaces the previous ROM but does not require a
new PBW.

In a per-ROM stock build, the cartridge is packaged as a PBW resource instead.
That PBW is personal to the supplied ROM and has no runtime downloader.

## Firmware and audio

The universal build uses ordinary AppMessage for transfer, plus a CFW-only
app-blob API to persist and read up to 8 MB from watch flash. Its CPU load
exposes speaker refill starvation in the stock scheduler.

Prebuilt DVT and PVT firmware containing the app-blob API and speaker
scheduling/DMA fixes is published from the
[ShinkoNet PebbleOS fork](https://github.com/ShinkoNet/PebbleOS/releases). The
[firmware notes](https://github.com/ShinkoNet/PebbleOS/blob/main/PEBBLEBOY_FIRMWARE.md)
explain hardware selection, sideload precautions, source patches, and the
automatic upstream-sync process. Each release contains a merged PBZ with both
firmware slots; users do not need to compile PebbleOS themselves.

The current Time 2 speaker API accepts Pebbleboy's mono signed 16 kHz/16-bit
PCM format. Disabling audio reduces CPU load; no larger app-memory allocation
is required.

## Controls

During a game:

| Pebble input | Game Boy input |
| --- | --- |
| Select button | A |
| Down button | B |
| Up button | Start |
| Back click | Select |
| Touchscreen direction | D-pad |

To toggle best-effort 2× fast-forward, press the Game Boy Start+Select chord
twice within one second (hold the watch's Up button and double-click Back).
Fast-forward renders at up to 30 FPS and pauses audio until normal speed is
restored.

## Build

With the Pebble SDK active, a normal SDK build remains useful for development:

```sh
pebble build
```

That produces a ROM-free build without the CFW blob API. To build the universal
CFW target, point the build at an SDK generated from the matching firmware:

```sh
PEBBLEBOY_CFW_SDK=/path/to/generated-sdk/emery pebble build
```

For development only, a ROM can still be embedded at
`resources/data/cartridge.gb`:

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
