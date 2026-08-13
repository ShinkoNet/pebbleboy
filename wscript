#
# Pebble application build rules.
#
import copy
import os.path

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
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        # Until the blob and speaker fixes land upstream, every build targets
        # Pebbleboy CFW's ABI. The emulator memory/performance profile itself
        # is unified and fits the standard 128 KiB app region.
        ctx.env.SDK_VERSION_MINOR = 0x6b
        ctx.env.append_unique('DEFINES', 'PEBBLEBOY_CFW_OFFICIAL_SDK_BRIDGE=1')
        ctx.env.append_value(
            'CFLAGS', '-I{}'.format(ctx.path.find_dir('src/c').abspath()))
        if embedded:
            # Personal PBWs carry an immutable cartridge resource and do not
            # need the runtime blob loader.
            ctx.env.append_unique('DEFINES', 'PEBBLEBOY_EMBEDDED_ROM_BUILD=1')
        if embedded:
            # App resources live in Obelix's large PFS. This build-time limit
            # supports the complete 8 MiB expanded MBC5 cartridge format.
            ctx.env.PLATFORM = dict(ctx.env.PLATFORM)
            ctx.env.PLATFORM['MAX_RESOURCES_SIZE'] = 0x810000
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
