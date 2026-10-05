#!/usr/bin/env python3
# Copyright 2026 CEMAXECUTER LLC
#
# Cuts an ESP32 app image out of a full flash dump (such as backup/stock.bin)
# so it can go into a HaLowScope firmware slot:
#
#   python3 tools/app_from_flash.py backup/stock.bin stock-camera.bin
#
# The app is read from offset 0x10000 (the stock layout's factory slot) by
# default; the image's own headers say where it ends.

import argparse
import struct
import sys


def image_length(data):
    if len(data) < 24 or data[0] != 0xE9:
        sys.exit('no app image at that offset (no 0xE9 header)')
    segments = data[1]
    pos = 24
    for _ in range(segments):
        if pos + 8 > len(data):
            sys.exit('image runs past the end of the dump')
        _, length = struct.unpack_from('<II', data, pos)
        pos += 8 + length
    pos = (pos + 1 + 15) & ~15     # checksum byte, padded to 16
    if data[23] == 1:              # SHA-256 appended
        pos += 32
    return pos


def main():
    p = argparse.ArgumentParser(description='Cuts an ESP32 app image out of a full flash dump.')
    p.add_argument('dump')
    p.add_argument('out')
    p.add_argument('--offset', type=lambda v: int(v, 0), default=0x10000)
    a = p.parse_args()
    with open(a.dump, 'rb') as f:
        f.seek(a.offset)
        data = f.read(0x1000000)
    n = image_length(data)
    desc = data[32:32 + 256]
    if struct.unpack_from('<I', desc)[0] == 0xABCD5432:
        project = desc[48:80].split(b'\0')[0].decode(errors='replace')
        version = desc[16:48].split(b'\0')[0].decode(errors='replace')
        print(f'{project} {version}, {n} bytes')
    else:
        print(f'{n} bytes (no app description)')
    with open(a.out, 'wb') as f:
        f.write(data[:n])


if __name__ == '__main__':
    main()
