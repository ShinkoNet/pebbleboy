#
# Pebble application build rules.
#
import copy
import os.path

from waflib import Errors

top = '.'
out = 'build'

LOCAL_ROM_PATH = 'resources/data/cartridge.gb'
LOCAL_ROM_RESOURCE = {
    'type': 'raw',
    'name': 'CARTRIDGE',
    'file': 'data/cartridge.gb',
}


def embed_local_rom():
    # Release builds are deliberately ROM-free. Developers can opt into the
    # legacy resource-backed path without maintaining a separate manifest.
    return os.environ.get('PEBBLEBOY_EMBED_ROM') == '1' and os.path.exists(LOCAL_ROM_PATH)


def build_target(embedded):
    target = os.environ.get('PEBBLEBOY_BUILD_TARGET', '')
    if not target:
        # Preserve the historical command-line behaviour: ordinary builds are
        # the ROM-free CFW loader, while PEBBLEBOY_EMBED_ROM builds are stock.
        return 'stock' if embedded else 'cfw'
    if target not in ('stock', 'cfw'):
        raise Errors.WafError('PEBBLEBOY_BUILD_TARGET must be stock or cfw')
    if target == 'stock' and not embedded:
        raise Errors.WafError('the stock target requires an embedded ROM')
    return target


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    ctx.load('pebble_sdk')

    if embed_local_rom():
        for env in ctx.all_envs.values():
            if not env.PLATFORM_NAME or not env.PROJECT_INFO:
                continue
            env.PROJECT_INFO = copy.deepcopy(env.PROJECT_INFO)
            env.RESOURCES_JSON = list(env.RESOURCES_JSON)
            env.RESOURCES_JSON.append(LOCAL_ROM_RESOURCE)
            env.PROJECT_INFO['resources']['media'] = env.RESOURCES_JSON


def build(ctx):
    ctx.load('pebble_sdk')

    build_worker = os.path.exists('worker_src')
    binaries = []

    cached_env = ctx.env
    embedded = embed_local_rom()
    target = build_target(embedded)
    cfw_build = target == 'cfw'
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        if cfw_build:
            ctx.env.SDK_VERSION_MINOR = 0x6b
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE=1')
            # CFLAGS precede the SDK's generated include path. This lets
            # pebble_process_info.h stamp the CFW ABI version while still
            # delegating every ordinary SDK declaration with include_next.
            ctx.env.append_value(
                'CFLAGS',
                '-I{}'.format(ctx.path.find_dir('src/c').abspath()))
        if embedded:
            # Suppress the source-level ROM-free default for personal PBWs
            # that carry an immutable cartridge resource. This is independent
            # of whether the personal build targets stock firmware or CFW.
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_EMBEDDED_ROM_BUILD=1')
        if target == 'stock':
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_STOCK_BUILD=1')
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_NO_AUDIO=1')
        # The emulator's CPU, LCD and mixer loops are throughput-bound. The
        # SDK defaults to -Os; the 128 KiB target still benefits from selective
        # speed-oriented compilation while remaining within its app region.
        if '-O3' not in ctx.env.CFLAGS:
            ctx.env.append_value('CFLAGS', '-O3')
        if embedded:
            # App resources live in Obelix's large PFS. The stock 1 MiB SDK
            # ceiling is a build-time limit. Stock personal builds support a
            # standard 4 MiB cartridge; CFW personal builds also support the
            # 8 MiB MBC5-expanded cartridge format.
            ctx.env.PLATFORM = dict(ctx.env.PLATFORM)
            ctx.env.PLATFORM['MAX_RESOURCES_SIZE'] = (0x810000 if cfw_build else 0x410000)
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')

        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            binaries.append({'platform': platform, 'app_elf': app_elf, 'worker_elf': worker_elf})
            ctx.pbl_build(source=ctx.path.ant_glob('worker_src/c/**/*.c'),
                          target=worker_elf,
                          bin_type='worker')
        else:
            binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env

    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries,
                   js=ctx.path.ant_glob(['src/pkjs/**/*.js',
                                         'src/pkjs/**/*.json',
                                         'src/common/**/*.js']),
                   js_entry_file='src/pkjs/index.js')
