#!/usr/bin/env python3
"""Write a LisaWrite (UnitFf v5) document file from plain text: the reference
for tool/lisawrite.c, checked in granny-smith with LisaWrite 3.1 (it opens the
file, and Save & Put Away writes the same structure back).

    lwmake.py in.txt out   (paragraphs = lines; all formats standard)

Mirrors WriteIcsFf/FBuildPgff in LIBSU/UNITFF: header "\r", footer "\r",
then the body; one paragraph run per paragraph start (ofbc 97 = standard),
one character run (ofbc 97), the vanish run at index 251, the trailer.
"""
import struct, sys

CB_FREE = 504          # cbFreePg
OFBC_STD, OFBC_VAN, OFBC_PARDUMMY = 97, 112, 127


def run(ofbc, ich):
    return (ofbc << 9) | ich


def build(text):
    """text: bytes of the whole lp stream (hdr+ftr+body), every para ends in CR."""
    pages = []          # (chars, parstarts(list of ich), dummy?, clpPrv, clpNxt)
    pos = 0
    n = len(text)
    par_starts = {0}
    for i, c in enumerate(text):
        if c == 13 and i + 1 < n:
            par_starts.add(i + 1)
    lp_of = {}          # text offset -> lp
    raw = []
    while pos < n:
        # capacity: chars + (npar + nch + 2) * 2 < 504, nch = 1, npar counted per page
        k = 0
        npar = 1        # a run at ich 0 always (start or continuation)
        while pos + k < n:
            add = 1 if (pos + k + 1) in par_starts and k + 1 < n - pos else 0
            # chars + (par runs + dummy reserve + char run + van run) * 2
            if (k + 1) + (npar + add + 3) * 2 >= CB_FREE:
                break
            k += 1
            npar += add
        chunk = text[pos:pos + k]
        for j in range(k):
            lp_of[pos + j] = len(raw) * 512 + j
        raw.append((pos, chunk))
        pos += k
    lp_of[n] = (len(raw) - 1) * 512 + len(raw[-1][1])
    # paragraph start lps (as LisaWrite: a page ending in CR starts a para at ichLim)
    starts = []
    for ipg, (p0, chunk) in enumerate(raw):
        for j in range(len(chunk)):
            if (p0 + j) in par_starts:
                starts.append(ipg * 512 + j)
        if chunk[-1:] == b'\r' and p0 + len(chunk) < n:
            starts.append(ipg * 512 + len(chunk))       # dummy run in the vanish area
    out = []
    for ipg, (p0, chunk) in enumerate(raw):
        L = len(chunk)
        lp0 = ipg * 512
        par = [j for j in range(L) if (p0 + j) in par_starts]
        dummy = chunk[-1:] == b'\r'
        if 0 not in par:
            par = [0] + par                         # continuation of previous para
        runs_par = [run(OFBC_STD, j) for j in par]
        if dummy:
            runs_par.append(run(OFBC_PARDUMMY, L))
        runs_ch = [run(OFBC_STD, 0)]
        prev = max([s for s in starts if s <= lp0], default=0)
        clpPrv = lp0 - prev
        nxt = [s for s in starts if s >= lp0 + 512]
        clpNxt = (nxt[0] - (lp0 + 512)) if nxt else 0
        irunChFst = 251 - len(runs_ch)
        irunParFst = irunChFst - len(runs_par)
        pg = bytearray(512)
        pg[:L] = chunk
        for i, r in enumerate(runs_par + runs_ch + [run(OFBC_VAN, L)]):
            struct.pack_into('>H', pg, (irunParFst + i) * 2, r)
        struct.pack_into('>hhhh', pg, 504, clpPrv, clpNxt, irunParFst, irunChFst)
        assert L + (len(runs_par) + len(runs_ch) + 2) * 2 <= CB_FREE
        out.append(bytes(pg))
    return out, lp_of


def leader(lpHdr, lpFtr, lpTxt):
    b = bytearray(512)
    struct.pack_into('>HHlll', b, 0, 0xCCAA, 5, lpHdr, lpFtr, lpTxt)
    b[16] = 0           # dummyLpLimFf (unused)
    b[17] = 0           # tyrlu = English
    # pglp: icsHdr, icsFtr (ignored), yTopHdr, dyHdr, dyHtBody, dyBotBody, tyxy
    struct.pack_into('>BBhhhhB', b, 0x12, 0, 0, 36, 36, 648, 684, 0)
    # prprf at 0x1E left zero: PrVersion 0 <> 11 -> FPrPrfValid false -> PrPrfDefault
    return bytes(b)


def main():
    body = open(sys.argv[1], 'rb').read().replace(b'\r\n', b'\n').replace(b'\n', b'\r')
    if not body.endswith(b'\r'):
        body += b'\r'
    text = b'\r' + b'\r' + body          # header "\r", footer "\r", body
    pages, lp_of = build(text)
    data = leader(lp_of[0], lp_of[1], lp_of[2]) + b''.join(pages)
    open(sys.argv[2], 'wb').write(data)


main()
