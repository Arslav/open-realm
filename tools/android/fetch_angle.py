#!/usr/bin/env python3
"""Prepare pinned ARM64 ANGLE libraries; keep the older AOSP trial explicitly selectable."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / 'build/android/angle/sdk35/arm64-v8a'
SDK_FINGERPRINT = 'Android/sdk_phone64_arm64/emu64a:15/AE3A.240806.019/12368160:userdebug/test-keys'
FILES = {
    'libEGL_angle.so': '244fa6f8fb33081298df4bcf878d3fa33ce90c10d8fbff0c3f2f03a22c1a1411',
    'libGLESv2_angle.so': 'b95da08d0dc11ffc2cd56685a17a9cdd45ae7113b07e04b504693476eb72d693',
}
REVISION = '9a0834cc8a5f3427dae2f6e87de4dfae50cb781c'
REPOSITORY = 'https://android.googlesource.com/platform/packages/modules/ANGLE/'
LEGACY_FILES = {
    'libEGL_angle.so': 'cb93be360bb9fd727c469f0f7abbcdd2b751b2a1c9bfabb33b09093849455c0e',
    'libGLESv2_angle.so': '43fe2374f3361ac26c8597f4866efed8205fb223ece2d14c857af645182d9ddc',
}


def verify(data, name, expected):
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError(f'ANGLE hash mismatch: {name}; the cache/source is not the pinned library')
    if len(data) < 20 or data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
        raise RuntimeError(f'ANGLE is not an ARM64 ELF library: {name}')


def fetch(out=CACHE, source=None, legacy=False):
    out, source = Path(out), Path(source) if source else None
    files = LEGACY_FILES if legacy else FILES
    # Validate the complete input before replacing anything in a prior verified cache.
    inputs, records = {}, []
    for name, expected in files.items():
        target = out / name
        url = f'{REPOSITORY}+/{REVISION}/lib/arm64/{name}?format=TEXT'
        if source:
            data = (source / name).read_bytes()
        elif target.exists():
            data = target.read_bytes()
        elif legacy:
            with urllib.request.urlopen(url, timeout=120) as response:
                data = base64.b64decode(response.read().strip(), validate=True)
        else:
            raise RuntimeError('Missing pinned SDK35 ANGLE. Import once with tools/android/extract_angle.py '
                               '--serial emulator-5554; see packaging/android/angle/README.md. '
                               'Use --without-angle for a GLES-only APK.')
        verify(data, name, expected)
        inputs[name] = data
        records.append({'file': name, 'sha256': expected, 'bytes': len(data)})
    out.mkdir(parents=True, exist_ok=True)
    for name, data in inputs.items():
        target = out / name
        if not target.exists() or target.read_bytes() != data:
            temporary = target.with_suffix('.download')
            temporary.write_bytes(data)
            temporary.replace(target)
    manifest = {'flavor': 'legacy-aosp' if legacy else 'sdk35', 'files': records}
    if legacy:
        manifest.update(revision=REVISION, angle_source_revision='3f647b1bf9a4',
                        repository=REPOSITORY, elf_load_alignment=4096,
                        scope='Historical regression control; not a current upstream ANGLE build')
    else:
        manifest.update(sdk_package='system-images;android-35;default;arm64-v8a', sdk_revision=2,
                        fingerprint=SDK_FINGERPRINT, elf_load_alignment=16384,
                        scope='Official SDK image prebuilt; embedded ANGLE git hash is unknown. '
                              'Verified on Android 12 Honor; not a claim about every Android API/device.')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--from-dir', type=Path, help='import the exact pinned libraries from this directory')
    parser.add_argument('--legacy', action='store_true', help='retain the historical 4 KiB AOSP experiment')
    args = parser.parse_args()
    out = args.out or (ROOT / 'build/android/angle/legacy/arm64-v8a' if args.legacy else CACHE)
    print(fetch(out, args.from_dir, args.legacy))
