"""
PlatformIO post-build script: merge the bootloader, partition table, OTA data
and application into one image that esptool writes at offset 0x0.

Devices that Inky does not list (Metalio E-Ink 4, EEGO Reader A4) have no
browser installer, and `firmware-<device>.bin` alone is only an application
image: a reader that has never run CrossInk also needs the bootloader and
partition table. Enable per environment with `custom_firmware_full_image = yes`.

Output: .pio/build/<env>/firmware-<device>-full.bin
Install: esptool --chip esp32s3 write-flash 0x0 firmware-<device>-full.bin
"""

import os
import subprocess
import sys


def _enabled(env):
    try:
        flag = env.GetProjectOption('custom_firmware_full_image')
    except Exception:
        return False
    return str(flag).strip().lower() in {'1', 'true', 'yes', 'on'}


def merge_full_image(source, target, env):
    del source
    if not _enabled(env):
        return
    app = str(target[0])
    build_dir = os.path.dirname(app)
    try:
        device_type = env.GetProjectOption('custom_firmware_device_type') or env['PIOENV']
    except Exception:
        device_type = env['PIOENV']
    output = os.path.join(build_dir, f'firmware-{device_type}-full.bin')

    images = []
    for offset, image in env.get('FLASH_EXTRA_IMAGES', []):
        images += [env.subst(str(offset)), env.subst(str(image))]
    images += [env.subst('$ESP32_APP_OFFSET') or '0x10000', app]
    if len(images) < 4:
        raise RuntimeError('Full image needs the bootloader and partition table from FLASH_EXTRA_IMAGES')

    board = env.BoardConfig()
    esptool = env.subst('$OBJCOPY').strip('"')
    common = [
        esptool, '--chip', board.get('build.mcu', 'esp32s3'),
    ]
    options = [
        '--flash-mode', env.subst('${__get_board_flash_mode(__env__)}') or board.get('build.flash_mode', 'dio'),
        '--flash-freq', env.subst('${__get_board_f_image(__env__)}') or '80m',
        '--flash-size', board.get('upload.flash_size', '16MB'),
        '-o', output,
    ]
    # esptool 5 spells the command merge-bin; esptool 4 used merge_bin.
    for command in ('merge-bin', 'merge_bin'):
        result = subprocess.run(common + [command] + options + images, check=False)
        if result.returncode == 0:
            print(f'Full flash image written to: {output}')
            return
    raise RuntimeError(f'esptool could not merge the full flash image for {device_type}')


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        merge_full_image,
    )
except NameError:
    print('merge_full_image.py: must be run via PlatformIO', file=sys.stderr)
