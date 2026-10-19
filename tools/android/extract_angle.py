#!/usr/bin/env python3
"""Import the pinned ANGLE from an already running official SDK emulator, read-only."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from fetch_angle import CACHE, ROOT, SDK_FINGERPRINT, FILES, fetch, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--sdk', type=Path, default=ROOT / 'build/android/sdk')
    parser.add_argument('--out', type=Path, default=CACHE)
    args = parser.parse_args()
    adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
    fingerprint = subprocess.check_output(adb + ['shell', 'getprop', 'ro.build.fingerprint'], timeout=30).decode().strip()
    if fingerprint != SDK_FINGERPRINT:
        raise RuntimeError(f'ANGLE source image mismatch: {fingerprint}; expected {SDK_FINGERPRINT}')
    with tempfile.TemporaryDirectory(prefix='openrealm-angle-') as temporary:
        for name, expected in FILES.items():
            data = subprocess.check_output(adb + ['exec-out', 'cat', '/system/lib64/' + name], timeout=60)
            verify(data, name, expected)
            (Path(temporary) / name).write_bytes(data)
        print(fetch(args.out, temporary))


if __name__ == '__main__':
    main()
