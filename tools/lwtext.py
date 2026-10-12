#!/usr/bin/env python3
"""Extract the body text of a LisaWrite (UnitFf) document file as plain text
(the reference for lw_read in tool/lisawrite.c)."""
import struct, sys

def lw_text(f):
    pw, ver, lpHdr, lpFtr, lpTxt = struct.unpack_from('>HHlll', f, 0)
    if pw != 0xCCAA:
        raise ValueError('not a LisaWrite file')
    parts = {'hdr': bytearray(), 'ftr': bytearray(), 'body': bytearray()}
    for ipg in range(len(f) // 512 - 1):
        pg = f[512 * (ipg + 1):512 * (ipg + 2)]
        ichLim = struct.unpack_from('>H', pg, 502)[0] & 0x1FF     # argRun[251].ichFst
        for ich in range(ichLim):
            lp = ipg * 512 + ich
            k = 'hdr' if lp < lpFtr else 'ftr' if lp < lpTxt else 'body'
            parts[k].append(pg[ich])
    return parts

if __name__ == '__main__':
    p = lw_text(open(sys.argv[1], 'rb').read())
    sys.stdout.write(bytes(p['body']).decode('mac_roman').replace('\r', '\n'))
