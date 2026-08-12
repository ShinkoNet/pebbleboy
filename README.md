# Pebbleboy

Pebbleboy is a Game Boy and Game Boy Color emulator for Pebble Time 2 and
Pebble 2 Duo. It uses Peanut-GB for the CPU and LCD core and can use a native
16 kHz/16-bit PCM mixer on speaker-equipped watches. The emulator uses 128 KiB
app limit, so it can't be installed on older Pebbles. Pebble 2 Duo runs GBC
games with monochrome output.

Pebbleboy does not contain or distribute commercial Game Boy ROMs. You must
provide ROM images that you are legally entitled to use.

## Release types

Pebbleboy is currently being developed around two complementary release types:

| Release | ROM delivery | Firmware | Audio |
| --- | --- | --- | --- |
| CFW build (Loader/ROM chooser) | Configure a URL; the phone downloads the selected ROM once and installs it into watch flash | Sideloaded Firmware | Yes |
| Stock build (Preloaded ROM) | Run the build script yourself to supply your ROM to build a personal .PBW with the game inside | Stock-compatible target | No |

The universal build needs CFW because it needs code inside Pebble OS to overwrite
its own app storage in an area where Pebbleboy has pre-allocated it, as part of the
app install size. The stock build can simply read from the built PBW resource after
the game has been packaged to it and avoid the blob API that CFW uses entirely.

About audio: Only Pebble Time 2 and Pebble 2 Duo have speakers. The CFW has fixes
to the audio that I could probably submit PRs upstream for (I'm less confident
they'll accept the functionality for an app to have its own writable blob
filesystem API tho). These Speaker and scheduler improvements are CFW features
unless equivalent support lands upstream. Since audio is pretty much dependent on
these fixes, audio only works under my sideloaded firmware.

## Install and configure

The file `Pebbleboy.pbw` in releases is the ROM-free CFW build and can be shared.
**YOU WILL NEED TO SIDELOAD THE FIRMWARE TO RUN IT**

If you don't want to deal with flashing via Core Device's app for whatever reason,
including just not trusting unofficial builds (good on you btw, you're sane) then
you can package ROMs without audio support by building it yourself for stock.
This also will perform worse as you can't cache as many banks as the CFW build can.
It'll be constantly reading from the pebble's flash in the overworld in pokecrystal.

CloudPebble instances will only build the loader under CFW build flags, not stock!

Anyway...

1. Sideload the firmware and connect the watch to its companion phone.
2. Open Pebbleboy's settings in the Pebble mobile app.
3. Add one or more game names and direct-download ROM URLs (or base64 text URLs).
4. Choose display scaling and whether speaker audio is enabled, then save.
5. Launch Pebbleboy and choose a game with **Up/Down** and **Select**. The phone
   transfers it once; subsequent launches read it directly from watch flash.

Each URL must return either a binary `.gb`/`.gbc` file or plain base64. Indirect downloads
from random ROM website's won't work. I won't tell you where to find ROMs that
are hosted in this way, unfortunately...

If hosting a distribution URL yourself for your homebrew roms for example, the
host must allow the Pebble phone JavaScript to fetch the URL. HTTPS and an
`Access-Control-Allow-Origin: *` response header are recommended.

The settings editor is hosted at `https://ptv.netcavy.net/gb/`. Existing ROM
names and URLs are passed to it in the URL fragment, which browsers do not send
to the web server. The static source for the page is in `config/index.html`.

The library supports up to 12 entries. One selected ROM is installed on the
watch at a time. The phone may also cache the active download. Save RAM belongs
to the locally installed cartridge, so gameplay and saving do not depend on a
Bluetooth connection. I plan to code a feature for you to archive your SRAM if
you change roms, right now it just overwrites a single slot...

**So for now, expect your save to no longer function after you change the rom!!**

## For nerds

## Where large ROMs live

In the CFW build, the phone fetches the selected 32 KB–8 MB cartridge
and transfers it in checked AppMessage chunks. I'm so sorry. CFW then stores it
in a filesystem blob owned by Pebbleboy's UUID. This is a CFW-unique app-blob API
to persist and read up to 8 MB from watch flash. The blob is committed only
after its size and CRC32 verify so the transfer knows it got everything.
The emulator then reads cartridge banks locally from flash through its RAM
cache. Installing another game replaces the previous ROM and SRAM but does not
require a new PBW.

In a personal stock or CFW build, the cartridge is packaged as a PBW resource
instead. That PBW is personal to the supplied ROM and has no runtime downloader.
It still reads the ROM and SRAM from flash. The CFW build retains its larger
cache and audio; the stock build uses the smaller cache without audio.

## Firmware and audio

Prebuilt DVT and PVT firmware containing the app-blob API and speaker
scheduling/DMA fixes is published from my
[PebbleOS fork](https://github.com/ShinkoNet/PebbleOS/releases). The
[firmware notes](https://github.com/ShinkoNet/PebbleOS/blob/main/PEBBLEBOY_FIRMWARE.md)
explain hardware selection, sideload precautions, source patches, and the
automatic upstream-sync process. Each release contains a merged PBZ with both
firmware slots done by github actions for each upstream release, but feel free
to compile it yourself if you want to feel extra safe after reading the code
yourself :)
I accept no liabilities if my app rewrites something it wasn't supposed to when
loading your ROM into it.

The current Time 2 speaker API accepts Pebbleboy's mono signed 16 kHz/16-bit
PCM format. Disabling audio reduces CPU load so it might improve performance,
but most issues are just a bottleneck from the flash or screen scaling.

## Controls

During a game:

| Pebble input | Game Boy input |
| --- | --- |
| Select button | A |
| Down button | B |
| Up button | Start |
| Back click | Select |
| Touchscreen direction | D-pad |

To toggle best-effort 2× fast-forward, press the Game Boy Start+Select twice
within one second (hold the watch's Up button and double-click Back).
Fast-forward pauses audio until normal speed is restored.

## Build

You need the latest official Pebble SDK. To build the universal CFW loader:

```sh
pebble build
```

Most users building on Linux can package a ROM from their own filesystem into
a personal PBW by choosing either the stock or CFW target:

```sh
tools/build-rom.sh /path/to/game.gbc --target stock
tools/build-rom.sh /path/to/game.gbc --target cfw
```

The script uses the normally installed `pebble` command and active official SDK
from `PATH`. It builds in a temporary directory and writes the finished PBW to
`dist/` by default. Pass `--output FILE` to choose another destination.

The stock target supports ROMs up to 4 MB, uses a 7 KiB cartridge cache, and
has no audio. The CFW target supports ROMs up to 8 MB, uses a 24 KiB cache, and
has audio.

Please ensure you are allowed to distribute the ROM you bundle with Pebbleboy
if you plan to share your own release.

For low-level development, the equivalent manual embedded-ROM environment is
`PEBBLEBOY_EMBED_ROM=1 PEBBLEBOY_BUILD_TARGET=stock|cfw pebble build`, with the
ROM at `resources/data/cartridge.gb`.

## Credits

The emulator core is based on Peanut-GB by Mahyar Koshkouei and contains
credited MIT-licensed portions from SameBoy. See `src/c/peanut_gb.h` for its
licence and notices.

## Licence

Pebbleboy is distributed under the [MIT License](LICENSE). Third-party
copyright and licence information is collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and retained in the relevant
source files.
