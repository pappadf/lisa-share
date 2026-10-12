#!/usr/bin/env python3
"""Scale a Lisa screenshot to the Lisa screen's proportions, for docs/.

    aspect.py in.png out.png

The emulator saves the 720 x 364 screen one pixel per Lisa pixel, but a Lisa
pixel is about 1.5 times taller than wide (90 x 60 dots per inch), so the
picture looks squashed.  This draws each pixel 2 wide and 3 tall (1440 x
1092) as a 1-bit grayscale PNG, as LisaPaint's README screenshots are.  Any
non-interlaced 8-bit PNG (gray, RGB or RGBA) is read; darker than 50% gray is
black.  Only the standard library is needed.
"""
import struct
import sys
import zlib

SX, SY = 2, 3


def chunks(data):
    p = 8
    while p < len(data):
        n, = struct.unpack('>I', data[p:p + 4])
        yield data[p + 4:p + 8], data[p + 8:p + 8 + n]
        p += 12 + n


def unfilter(raw, w, h, bpp):
    stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return rows


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG')
    idat = b''
    for kind, body in chunks(data):
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or bpp is None:
        sys.exit(f'{path}: only non-interlaced 8-bit gray, RGB or RGBA PNGs')
    rows = unfilter(zlib.decompress(idat), w, h, bpp)
    chans = 1 if ctype in (0, 4) else 3
    return w, h, [[sum(r[x * bpp:x * bpp + chans]) // chans < 128 for x in range(w)] for r in rows]


def write_png(path, w, h, black):
    W, H = w * SX, h * SY
    raw = bytearray()
    for row in black:
        bits = bytearray((W + 7) // 8)
        for x, b in enumerate(row):
            if not b:                   # 1 = white in a 1-bit gray PNG
                for k in range(SX):
                    X = x * SX + k
                    bits[X >> 3] |= 0x80 >> (X & 7)
        raw += (b'\0' + bits) * SY

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 1, 0, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    w, h, black = read_png(sys.argv[1])
    write_png(sys.argv[2], w, h, black)


if __name__ == '__main__':
    main()
