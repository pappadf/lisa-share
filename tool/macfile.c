/* Mac file documents: a file copied from a server as it is.
 *
 * The document's files, under its prefix -<volume>-{DnT22}:
 *   DATA   the data fork
 *   RSRC   the resource fork (only when it is not empty)
 *   INFO   a macfile_info record: the Mac name, Finder info (type, creator,
 *          flags), the fork lengths, and where it came from
 * so that it can go back to a Mac unchanged, and be converted on the Lisa
 * (Convert to...).  Its window shows the information and, for a text file,
 * the start of the text. */
#include <stdio.h>
#include <string.h>
#include "lisashare.h"

enum { M_DREAD = 1, M_DWRITE = 2, IO_SEQUENTIAL = 2, IO_ABSOLUTE = 0 };
#define INFO_MAGIC 0x4C534D46L          /* 'LSMF' */

typedef struct info_file {
    int32_t magic;
    int16_t version;
    macfile_info mi;
} info_file;

static void file_path(unsigned char *p, const char *prefix, const char *suffix)
{
    char s[256];

    snprintf(s, sizeof s, "%s%s", prefix, suffix);
    pstr(p, s);
}

static int read_info(const char *prefix, info_file *f)
{
    unsigned char path[256];
    Integer err, e2, refnum;
    int32_t got = 0;

    file_path(path, prefix, "INFO");
    lisa_open(&err, path, &refnum, M_DREAD);
    if (err > 0)
        return err;
    lisa_read_data(&err, refnum, f, sizeof *f, &got, IO_ABSOLUTE, 0);
    lisa_close_object(&e2, refnum);
    if (got != (int32_t)sizeof *f || f->magic != INFO_MAGIC)
        return 1;
    return 0;
}

int macfile_read_info(const char *prefix, macfile_info *mi)
{
    static info_file f;
    int err = read_info(prefix, &f);

    if (!err)
        *mi = f.mi;
    return err;
}

int macfile_open_fork(const char *prefix, int rsrc, int16_t *refnum)
{
    unsigned char path[256];
    Integer err;

    file_path(path, prefix, rsrc ? "RSRC" : "DATA");
    lisa_open(&err, path, refnum, M_DREAD);
    return err > 0 ? err : 0;
}

int32_t macfile_read(int16_t refnum, void *buf, int32_t len)
{
    Integer err;
    int32_t got = 0;

    lisa_read_data(&err, refnum, buf, len, &got, IO_SEQUENTIAL, 0);
    return got > 0 ? got : 0;
}

int macfile_is(const char *prefix)
{
    static info_file f;

    return read_info(prefix, &f) == 0;
}

static int create(const char *prefix, const char *suffix, int16_t *refnum)
{
    unsigned char path[256];
    Integer err;

    file_path(path, prefix, suffix);
    lisa_kill_object(&err, path);
    lisa_make_file(&err, path, 0);
    if (err > 0)
        return err;
    lisa_open(&err, path, refnum, M_DWRITE);
    return err > 0 ? err : 0;
}

int macfile_create_fork(const char *prefix, int rsrc, int16_t *refnum)
{
    return create(prefix, rsrc ? "RSRC" : "DATA", refnum);
}

int macfile_write(int16_t refnum, const void *data, int32_t len)
{
    Integer err;
    int32_t put;

    lisa_write_data(&err, refnum, data, len, &put, IO_SEQUENTIAL, 0);
    return err > 0 ? err : (put == len ? 0 : 1);
}

void macfile_close(int16_t refnum)
{
    Integer err;

    lisa_close_object(&err, refnum);
}

int macfile_write_info(const char *prefix, const macfile_info *mi)
{
    static info_file f;
    int16_t refnum;
    int err;

    memset(&f, 0, sizeof f);
    f.magic = INFO_MAGIC;
    f.version = 1;
    f.mi = *mi;
    if ((err = create(prefix, "INFO", &refnum)) != 0)
        return err;
    err = macfile_write(refnum, &f, sizeof f);
    macfile_close(refnum);
    return err;
}

/* Copy one of a document's files to the same suffix under a new prefix. */
static int copy_one(const char *from, const char *to, const char *suffix)
{
    static char buf[1024];
    unsigned char path[256];
    Integer err, e2, in;
    int16_t out;
    int32_t got;
    int e;

    file_path(path, from, suffix);
    lisa_open(&err, path, &in, M_DREAD);
    if (err > 0)
        return 0;                       /* no such file (an empty resource fork) */
    if ((e = create(to, suffix, &out)) != 0) {
        lisa_close_object(&e2, in);
        return e;
    }
    for (;;) {
        lisa_read_data(&err, in, buf, sizeof buf, &got, IO_SEQUENTIAL, 0);
        if (got <= 0)
            break;
        if ((e = macfile_write(out, buf, got)) != 0)
            break;
    }
    lisa_close_object(&e2, in);
    macfile_close(out);
    return e;
}

int macfile_copy(const char *from, const char *to)
{
    int e;

    if ((e = copy_one(from, to, "DATA")) != 0 || (e = copy_one(from, to, "RSRC")) != 0)
        return e;
    return copy_one(from, to, "INFO");
}

static void text_at(int h, int v, const char *s)
{
    unsigned char p[256];

    MoveTo((Integer)h, (Integer)v);
    pstr(p, s);
    DrawString(p);
}

void macfile_draw(WindowPtr w, const char *prefix)
{
    static info_file f;
    static char text[600];
    unsigned char path[256];
    Rect r = w->portRect;
    Integer err, e2, refnum;
    int32_t got = 0;
    char s[120];
    int v = r.top + 16, i, start;

    if (read_info(prefix, &f)) {
        text_at(r.left + 10, v, "This document's information cannot be read.");
        return;
    }
    snprintf(s, sizeof s, "Mac file \"%s\"", f.mi.name);
    text_at(r.left + 10, v, s);
    v += 16;
    if (f.mi.finder[0])
        snprintf(s, sizeof s, "Type %.4s, creator %.4s.  Data fork %lu bytes, resource fork %lu bytes.",
                 (const char *)f.mi.finder, (const char *)f.mi.finder + 4, (unsigned long)f.mi.data_len,
                 (unsigned long)f.mi.rsrc_len);
    else
        snprintf(s, sizeof s, "No type or creator.  Data fork %lu bytes, resource fork %lu bytes.",
                 (unsigned long)f.mi.data_len, (unsigned long)f.mi.rsrc_len);
    text_at(r.left + 10, v, s);
    v += 16;
    snprintf(s, sizeof s, "Copied from \"%s\" on %s.", f.mi.volume, f.mi.server);
    text_at(r.left + 10, v, s);
    v += 12;
    MoveTo(r.left, v);
    LineTo(r.right, v);
    v += 16;

    /* the start of the data fork, as text */
    file_path(path, prefix, "DATA");
    lisa_open(&err, path, &refnum, M_DREAD);
    if (err > 0)
        return;
    lisa_read_data(&err, refnum, text, sizeof text - 1, &got, IO_ABSOLUTE, 0);
    lisa_close_object(&e2, refnum);
    if (got < 0)
        got = 0;
    text[got] = 0;
    for (i = start = 0; i <= got && v < r.bottom - 6; i++) {
        char c = text[i];
        if (c == '\r' || c == '\n' || c == 0 || i - start >= 90) {
            text[i] = 0;
            text_at(r.left + 10, v, text + start);
            text[i] = c;
            v += 14;
            if (c == 0)
                break;
            start = (c == '\r' || c == '\n') ? i + 1 : i;     /* a long line wraps */
        } else if ((unsigned char)c < 32) {
            text[i] = ' ';
        }
    }
}
