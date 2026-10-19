#!/usr/bin/env python3
"""Build hash-pinned, decoder-only FFmpeg for Android or local movie verification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VERSION = '8.0.3'
SHA256 = '6136812ea6d4e68bdba27e33c2a94382711cdf4f8602ffef056ff792bd6f9818'
URL = f'https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz'
LIBRARIES = ('avutil', 'swresample', 'swscale', 'avcodec', 'avformat')


def build(ndk=None, jobs=3):
    cache = ROOT / 'build/deps'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f'ffmpeg-{VERSION}.tar.xz'
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=30) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError('FFmpeg source checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('FFmpeg cached source checksum mismatch')
    source = cache / f'ffmpeg-{VERSION}'
    if not source.is_dir():
        subprocess.run(['tar', '-xf', str(archive), '-C', str(cache)], check=True)
    output = ROOT / ('build/android/ffmpeg/arm64-v8a' if ndk else 'build/ffmpeg/host')
    work = output / 'build'
    work.mkdir(parents=True, exist_ok=True)
    flags = [f'--prefix={output}', '--disable-everything', '--disable-autodetect', '--disable-doc',
             '--disable-programs', '--disable-avdevice', '--disable-avfilter', '--disable-static',
             '--enable-shared', '--enable-pic', '--enable-small', '--enable-avcodec', '--enable-avformat',
             '--enable-swscale', '--enable-swresample', '--enable-protocol=file', '--enable-demuxer=avi,mp3',
             '--enable-decoder=mpeg4,mp3,mp3float,pcm_s16le,rawvideo', '--enable-parser=mpeg4video,mpegaudio',
             '--pkg-config=false']
    if ndk:
        host = 'darwin-x86_64' if os.uname().sysname == 'Darwin' else 'linux-x86_64'
        tool = Path(ndk).resolve() / 'toolchains/llvm/prebuilt' / host / 'bin'
        flags += ['--target-os=android', '--arch=aarch64', '--enable-cross-compile',
                  f'--cc={tool}/aarch64-linux-android26-clang', f'--ar={tool}/llvm-ar',
                  f'--ranlib={tool}/llvm-ranlib', f'--strip={tool}/llvm-strip', f'--nm={tool}/llvm-nm',
                  '--extra-ldflags=-Wl,-z,max-page-size=16384']
    else:
        flags += ['--cc=clang', '--enable-ffprobe']
    manifest = output / 'manifest.json'
    recipe = {'version': VERSION, 'source': URL, 'source_sha256': SHA256, 'configure': flags}
    suffix = '.dylib' if not ndk and os.uname().sysname == 'Darwin' else '.so'
    libraries = [output / 'lib' / f'lib{name}{suffix}' for name in LIBRARIES]
    if manifest.is_file() and json.loads(manifest.read_text()) == recipe and all(p.is_file() for p in libraries):
        return output
    subprocess.run([str(source / 'configure'), *flags], cwd=work, check=True)
    subprocess.run(['make', f'-j{jobs}'], cwd=work, check=True)
    subprocess.run(['make', 'install'], cwd=work, check=True)
    shutil.copyfile(source / 'COPYING.LGPLv2.1', output / 'COPYING.LGPLv2.1')
    manifest.write_text(json.dumps(recipe, indent=2) + '\n')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, help='Android NDK root; omit for a host verification build')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    print(build(args.ndk, args.jobs))
