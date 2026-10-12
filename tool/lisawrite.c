/* LisaWrite documents: plain text in, plain text out.
 *
 * A LisaWrite document (tool 1) is one file named exactly after the
 * document's prefix, {D<id>T1}, in the Office System's formatted-file format
 * (unit UnitFf, LIBSU/libsu-unitff): a 512-byte leader page, then 512-byte
 * text pages.
 *   - Leader: $CCAA, version 5, the lp addresses where the header, footer and
 *     body start (lp = text page * 512 + character), the page geometry
 *     (pglpStd: 36, 36, 648, 684) and print settings (zero: LisaWrite then
 *     uses the default printer's).
 *   - Text page: the characters from 0 (every paragraph ends in CR); a table
 *     of 16-bit runs (format offset << 9 | character index) ending at index
 *     251 with the "vanish" run, whose index is the page's character count;
 *     paragraph runs (one per paragraph start, and at 0 when the page
 *     continues a paragraph, and a dummy at the end when the page ends a
 *     paragraph) then one character run; and a trailer: the lp distances back
 *     to this page's paragraph start and on to the next page's, and where the
 *     runs start.
 * Every run here has format 97, the standard format, so the document takes
 * LisaWrite's defaults, like one torn off "LisaWrite Paper".  The header and
 * footer are empty paragraphs.  Checked in granny-smith with LisaWrite 3.1:
 * it opens such a document, and saves it back in the same form. */
#include <stdio.h>
#include <string.h>
#include "lisashare.h"

#define CB_FREE 504                     /* bytes per page for characters and runs */
#define OFBC_STD 97
#define OFBC_VAN 112
#define OFBC_DUMMY 127
#define MAXPAGES 256

static uint8_t pg[512];
static int32_t page_start[MAXPAGES], page_len[MAXPAGES];
/* The stream LisaWrite stores: the header "\r", the footer "\r", then the
   body (the caller's buffer, every paragraph ending in CR). */
static const uint8_t *body;
#define T(i) ((i) < 2 ? (uint8_t)'\r' : body[(i) - 2])

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
}

/* Does a paragraph start at position i of the stream? */
static int par_start(int32_t n, int32_t i)
{
    return i == 0 || (i < n && T(i - 1) == '\r');
}

static int32_t lp_of(int npages, int32_t pos)
{
    int k;

    for (k = 0; k < npages; k++)
        if (pos < page_start[k] + page_len[k])
            return (int32_t)k * 512 + (pos - page_start[k]);
    return (int32_t)(npages - 1) * 512 + page_len[npages - 1];
}

/* The body in place: every line end (CR, LF or CR LF) becomes CR, and the
   last paragraph gets one.  Returns the stream's length (the body's + 2). */
static int32_t make_stream(uint8_t *text, int32_t len)
{
    int32_t n = 0, i;
    uint8_t c, prev = 0;                /* prev: the original, text[i - 1] may be rewritten */

    for (i = 0; i < len; i++, prev = c) {
        c = text[i];
        if (c == '\n' && prev == '\r')
            continue;                   /* CR LF */
        text[n++] = c == '\n' ? '\r' : c;
    }
    if (n == 0 || text[n - 1] != '\r')
        text[n++] = '\r';
    body = text;
    return n + 2;
}

int lw_write(const char *path, uint8_t *text, int32_t len)
{
    int32_t n, pos, k, j, npar, add, lp0, lpnext, q;
    int npages = 0, p, i, nruns, nparruns;
    int16_t refnum;
    uint16_t runs[260];
    int err;

    n = make_stream(text, len);
    /* fill pages greedily: characters + (paragraph runs + dummy + character
       run + vanish run) * 2 must stay under 504 */
    for (pos = 0; pos < n && npages < MAXPAGES; pos += k) {
        k = 0;
        npar = 1;
        while (pos + k < n) {
            add = (par_start(n, pos + k + 1) && k + 1 < n - pos) ? 1 : 0;
            if ((k + 1) + (npar + add + 3) * 2 >= CB_FREE)
                break;
            k++;
            npar += add;
        }
        page_start[npages] = pos;
        page_len[npages++] = k;
    }
    if (pos < n)
        return ATK_E_FRAME;             /* too long */

    if ((err = lisadoc_create(path, 0, 0, &refnum)) != 0)
        return err;
    memset(pg, 0, sizeof pg);           /* the leader */
    put16(pg, 0xCCAA);
    put16(pg + 2, 5);
    put32(pg + 4, (uint32_t)lp_of(npages, 0));
    put32(pg + 8, (uint32_t)lp_of(npages, 1));
    put32(pg + 12, (uint32_t)lp_of(npages, 2));
    put16(pg + 0x14, 36);               /* yTopHdr, dyHdr, dyHtBody, dyBotBody */
    put16(pg + 0x16, 36);
    put16(pg + 0x18, 648);
    put16(pg + 0x1A, 684);
    err = macfile_write(refnum, pg, 512);

    for (p = 0; p < npages && !err; p++) {
        int32_t p0 = page_start[p], L = page_len[p];
        memset(pg, 0, sizeof pg);
        for (j = 0; j < L; j++)
            pg[j] = T(p0 + j);
        nruns = 0;
        if (!par_start(n, p0))
            runs[nruns++] = (uint16_t)(OFBC_STD << 9);      /* continues a paragraph */
        for (j = 0; j < L; j++)
            if (par_start(n, p0 + j))
                runs[nruns++] = (uint16_t)(OFBC_STD << 9 | j);
        if (T(p0 + L - 1) == '\r')
            runs[nruns++] = (uint16_t)(OFBC_DUMMY << 9 | L);
        nparruns = nruns;
        runs[nruns++] = (uint16_t)(OFBC_STD << 9);          /* the character run */
        runs[nruns++] = (uint16_t)(OFBC_VAN << 9 | L);      /* index 251 */
        for (i = 0; i < nruns; i++)
            put16(pg + (252 - nruns + i) * 2, runs[i]);

        /* clpPrv: back to the start of the paragraph this page starts in */
        lp0 = (int32_t)p * 512;
        for (q = p0; q > 0 && !par_start(n, q); q--)
            ;
        put16(pg + 504, (unsigned)(lp0 - lp_of(npages, q)));
        /* clpNxt: on from the next page's start to the next paragraph start,
           which may be the dummy at the end of a page that ends a paragraph */
        lpnext = 0;
        for (q = p0 + L; q < n && !par_start(n, q); q++)
            ;
        if (q < n) {
            int pq = (int)(lp_of(npages, q) / 512);
            lpnext = lp_of(npages, q);
            if (pq - 1 > p && page_start[pq] == q && T(q - 1) == '\r')
                lpnext = (int32_t)(pq - 1) * 512 + page_len[pq - 1];
            lpnext -= lp0 + 512;
        }
        put16(pg + 506, (unsigned)lpnext);
        put16(pg + 508, (unsigned)(252 - nruns));            /* irunParFst */
        put16(pg + 510, (unsigned)(252 - nruns + nparruns)); /* irunChFst */
        err = macfile_write(refnum, pg, 512);
    }
    macfile_close(refnum);
    return err;
}

int32_t lw_read(const char *path, uint8_t *out, int32_t max)
{
    char temp[256];
    int16_t refnum;
    int32_t lptxt, n = 0, ipg, ich, lim;

    /* a suspended document's state is in its TEMP file */
    snprintf(temp, sizeof temp, "%sTEMP", path);
    if (lisadoc_open(temp, &refnum) && lisadoc_open(path, &refnum))
        return -1;
    if (macfile_read(refnum, pg, 512) != 512 || pg[0] != 0xCC || pg[1] != 0xAA) {
        macfile_close(refnum);
        return -1;                      /* not a LisaWrite document */
    }
    lptxt = (int32_t)((uint32_t)pg[12] << 24 | (uint32_t)pg[13] << 16 | (uint32_t)pg[14] << 8 | pg[15]);
    for (ipg = 0; macfile_read(refnum, pg, 512) == 512; ipg++) {
        lim = (pg[502] << 8 | pg[503]) & 0x1FF;
        for (ich = 0; ich < lim && ich < CB_FREE; ich++)
            if (ipg * 512 + ich >= lptxt && n < max)
                out[n++] = pg[ich];
    }
    macfile_close(refnum);
    return n;
}
