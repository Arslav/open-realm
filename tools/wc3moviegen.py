#!/usr/bin/env python3
"""Generate an original three-frame AVI fixture, optionally with stereo PCM audio."""
import argparse
import math
from pathlib import Path
import struct


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes(len(data) & 1)


def movie(audio=False):
    width, height, count, rate = 32, 16, 3, 10
    pixels = width * height * 3
    header = struct.pack('<14I', 1000000 // rate, 0, 0, 0, count, 0, 2 if audio else 1,
                         pixels, width, height, 0, 0, 0, 0)
    video = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'DIB ', 0, 0, 0,
                        0, 1, rate, 0, count, pixels, 0xffffffff, 0, 0, 0, width, height)
    bitmap = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, pixels, 0, 0, 0, 0)
    streams = chunk(b'LIST', b'strl' + chunk(b'strh', video) + chunk(b'strf', bitmap))
    if audio:
        sound = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'auds', bytes(4), 0, 0, 0,
                            0, 4, 44100 * 4, 0, count * 4410, 17640, 0xffffffff, 4, 0, 0, 0, 0)
        wave = struct.pack('<HHIIHHH', 1, 2, 44100, 176400, 4, 16, 0)
        streams += chunk(b'LIST', b'strl' + chunk(b'strh', sound) + chunk(b'strf', wave))
    frames = b''
    for i, color in enumerate((b'\0\0\xff', b'\0\xff\0', b'\xff\0\0')):
        frames += chunk(b'00db', color * (width * height))
        if audio:
            pcm = b''.join(struct.pack('<hh', *(int(2000 * math.sin(2 * math.pi * 440 * (i * 4410 + n) / 44100)),) * 2)
                           for n in range(4410))
            frames += chunk(b'01wb', pcm)
    data = b'AVI ' + chunk(b'LIST', b'hdrl' + chunk(b'avih', header) + streams) + chunk(b'LIST', b'movi' + frames)
    return chunk(b'RIFF', data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--audio', action='store_true')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(movie(args.audio))
