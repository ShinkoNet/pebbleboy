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


def cfw_sdk_platform():
    path = os.environ.get('PEBBLEBOY_CFW_SDK', '')
    if not path:
        return None
    path = os.path.abspath(path)
    if not os.path.exists(os.path.join(path, 'include', 'pebble.h')):
        raise Errors.WafError('PEBBLEBOY_CFW_SDK must name a generated platform SDK directory')
    if not os.path.exists(os.path.join(path, 'lib', 'libpebble.a')):
        raise Errors.WafError('PEBBLEBOY_CFW_SDK is missing lib/libpebble.a')
    return path


def embed_local_rom():
    # Release builds are deliberately ROM-free. Developers can opt into the
    # legacy resource-backed path without maintaining a separate manifest.
    return os.environ.get('PEBBLEBOY_EMBED_ROM') == '1' and os.path.exists(LOCAL_ROM_PATH)


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
    custom_sdk = cfw_sdk_platform()
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        if custom_sdk:
            ctx.env.PEBBLE_SDK_PLATFORM = custom_sdk
            ctx.env.SDK_VERSION_MINOR = 0x6b
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_APP_BLOB=1')
        # The emulator's CPU, LCD and mixer loops are throughput-bound. The
        # SDK defaults to -Os; the 128 KiB target still benefits from selective
        # speed-oriented compilation while remaining within its app region.
        if '-O3' not in ctx.env.CFLAGS:
            ctx.env.append_value('CFLAGS', '-O3')
        if embed_local_rom():
            # App resources live in Obelix's large PFS. The stock 1 MiB SDK
            # ceiling is a build-time limit; leave enough room for a maximum
            # 4 MiB Game Boy ROM plus the fixed pbpack table.
            ctx.env.PLATFORM = dict(ctx.env.PLATFORM)
            ctx.env.PLATFORM['MAX_RESOURCES_SIZE'] = 0x410000
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
