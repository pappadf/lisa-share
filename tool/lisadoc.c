/* A Lisa document's files, for copying a document to a server as it is.
 *
 * A document is the files on its volume whose names begin with its prefix
 * {D<id>T<tool>}; each file can carry a label (up to 128 bytes kept with
 * the file, outside its data -- the Desktop Manager keeps its own
 * information in some).  lisadoc_list finds the files with RESET_CATALOG and
 * GET_NEXT_ENTRY; the rest reads and writes one file with its label. */
#include <stdio.h>
#include <string.h>
#include "lisashare.h"

enum { M_DREAD = 1, M_DWRITE = 2, IO_SEQUENTIAL = 2 };

/* "-vol-{D104T22}" -> vol "-vol", base "{D104T22}" */
static int split(const char *prefix, char *vol, char *base)
{
    const char *dash = strrchr(prefix, '-');

    if (!dash || dash == prefix)
        return 1;
    snprintf(vol, 64, "%.*s", (int)(dash - prefix), prefix);
    snprintf(base, 33, "%s", dash + 1);
    return 0;
}

int lisadoc_tool(const char *prefix)
{
    const char *t = strrchr(prefix, 'T');
    int n = 0;

    if (t)
        sscanf(t + 1, "%d", &n);
    return n;
}

int lisadoc_list(const char *prefix, char names[][33], int max)
{
    unsigned char vpath[80], pfx[40], entry[40];
    char vol[64], base[33];
    Integer err;
    int n = 0;

    if (split(prefix, vol, base))
        return 0;
    pstr(vpath, vol);
    pstr(pfx, base);
    lisa_reset_catalog(&err, vpath);
    if (err > 0)
        return 0;
    while (n < max) {
        lisa_get_next_entry(&err, pfx, entry);
        if (err > 0)
            break;
        cstr(names[n++], entry, 33);
    }
    return n;
}

int lisadoc_read_label(const char *path, uint8_t *label, int max)
{
    unsigned char p[256];
    Integer err;
    int32_t actual = 0;

    pstr(p, path);
    lisa_read_label(&err, p, label, max, &actual);
    return err > 0 ? 0 : (int)actual;
}

int lisadoc_create(const char *path, const uint8_t *label, int label_len, int16_t *refnum)
{
    unsigned char p[256];
    Integer err;
    int32_t actual;

    pstr(p, path);
    lisa_kill_object(&err, p);
    lisa_make_file(&err, p, (int16_t)(label_len > 0 ? label_len : 0));
    if (err > 0)
        return err;
    if (label_len > 0) {
        lisa_write_label(&err, p, label, label_len, &actual);
        if (err > 0)
            return err;
    }
    lisa_open(&err, p, refnum, M_DWRITE);
    return err > 0 ? err : 0;
}

int lisadoc_open(const char *path, int16_t *refnum)
{
    unsigned char p[256];
    Integer err;

    pstr(p, path);
    lisa_open(&err, p, refnum, M_DREAD);
    return err > 0 ? err : 0;
}
