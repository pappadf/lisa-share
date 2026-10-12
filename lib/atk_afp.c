/* AFP 2.0, the client calls lisa-share needs (AppleTalk Filing Protocol 2.1
 * and 2.2 spec, in gs-docs/library/network; granny-smith's server reference
 * docs/internals/core/network/appletalk_server.md).
 *
 * Each call builds its command block -- the AFP command code, then its
 * parameters, big-endian -- sends it with asp_command and decodes the reply.
 * They return the AFP result code (0 = NoErr; e.g. -5018 ObjectNotFound,
 * -5009 EOFErr) or an ATP/ASP error. */
#include <string.h>
#include "atk.h"

enum {
    FP_CLOSE_VOL = 2, FP_CLOSE_FORK = 4, FP_CREATE_DIR = 6, FP_CREATE_FILE = 7, FP_ENUMERATE = 9, FP_GET_SRVR_PARMS = 16,
    FP_LOGIN = 18, FP_LOGOUT = 20, FP_OPEN_VOL = 24, FP_OPEN_FORK = 26, FP_READ = 27, FP_SET_FILE_PARMS = 30,
    FP_WRITE = 33,
};
enum { PATH_LONG = 2 };
/* the parameters FPEnumerate asks for */
enum { P_CDATE = 1 << 2, P_MDATE = 1 << 3 };   /* both kinds: creation, modification date */
enum { F_FINDER = 1 << 5, F_LONGNAME = 1 << 6, F_FILENUM = 1 << 8, F_DATALEN = 1 << 9, F_RSRCLEN = 1 << 10 };
enum { D_LONGNAME = 1 << 6, D_DIRID = 1 << 8, D_OFFSPRING = 1 << 9 };

static uint8_t cmd[ATP_MAX_DATA];
static uint8_t reply[8 * ATP_MAX_DATA];

static int put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2;
}

static int put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
    return 4;
}

static unsigned get16(const uint8_t *p)
{
    return (unsigned)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static int put_pstring(uint8_t *p, const char *s)
{
    int n = (int)strlen(s);

    if (n > 255)
        n = 255;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, (size_t)n);
    return n + 1;
}

/* A Pascal string at `off` inside [base, base + size) into s (room for 32). */
static void get_name(const uint8_t *base, int size, int off, char *s)
{
    int n;

    s[0] = 0;
    if (off < 0 || off >= size)
        return;
    n = base[off];
    if (n > 32)
        n = 32;
    if (off + 1 + n > size)
        n = size - off - 1;
    memcpy(s, base + off + 1, (size_t)n);
    s[n] = 0;
}

int32_t afp_login(asp_session *s)
{
    int n = 0, len;

    cmd[n++] = FP_LOGIN;
    n += put_pstring(cmd + n, "AFPVersion 2.0");
    n += put_pstring(cmd + n, "No User Authent");
    return asp_command(s, cmd, n, reply, sizeof reply, &len);
}

int32_t afp_logout(asp_session *s)
{
    int len;

    cmd[0] = FP_LOGOUT;
    cmd[1] = 0;
    return asp_command(s, cmd, 2, reply, sizeof reply, &len);
}

int32_t afp_volumes(asp_session *s, char names[][28], int max, int *count)
{
    int32_t err;
    int len, n, i, p;

    *count = 0;
    cmd[0] = FP_GET_SRVR_PARMS;
    cmd[1] = 0;
    err = asp_command(s, cmd, 2, reply, sizeof reply, &len);
    if (err)
        return err;
    if (len < 5)
        return ATK_E_FRAME;
    s->server_time = get32(reply);
    n = reply[4];
    p = 5;
    for (i = 0; i < n && *count < max && p + 2 <= len; i++) {
        int l = reply[p + 1];           /* [flags, name] */
        if (l > 27)
            l = 27;
        memcpy(names[*count], reply + p + 2, (size_t)l);
        names[*count][l] = 0;
        (*count)++;
        p += 2 + reply[p + 1];
    }
    return 0;
}

int32_t afp_open_vol(asp_session *s, const char *name, uint16_t *vol_id)
{
    int32_t err;
    int n = 0, len;

    cmd[n++] = FP_OPEN_VOL;
    cmd[n++] = 0;
    n += put16(cmd + n, 1 << 5);        /* bitmap: the volume ID */
    n += put_pstring(cmd + n, name);
    err = asp_command(s, cmd, n, reply, sizeof reply, &len);
    if (err)
        return err;
    if (len < 4)
        return ATK_E_FRAME;
    *vol_id = (uint16_t)get16(reply + 2);
    return 0;
}

int32_t afp_close_vol(asp_session *s, uint16_t vol_id)
{
    int len;

    cmd[0] = FP_CLOSE_VOL;
    cmd[1] = 0;
    put16(cmd + 2, vol_id);
    return asp_command(s, cmd, 4, reply, sizeof reply, &len);
}

/* Decode one FPEnumerate structure's parameters (bitmap order). */
static void decode_entry(const uint8_t *base, int size, int is_dir, afp_entry *e)
{
    const uint8_t *p = base + 8;        /* names are at offsets from base */

    memset(e, 0, sizeof *e);
    e->is_dir = (uint8_t)is_dir;
    e->cdate = (int32_t)get32(base);    /* P_CDATE | P_MDATE first */
    e->mdate = (int32_t)get32(base + 4);
    if (is_dir) {                       /* D_LONGNAME | D_DIRID | D_OFFSPRING */
        get_name(base, size, (int)get16(p), e->name);
        e->id = get32(p + 2);
        e->offspring = (uint16_t)get16(p + 6);
    } else {                            /* F_FINDER | F_LONGNAME | F_FILENUM | F_DATALEN | F_RSRCLEN */
        memcpy(e->finder, p, 32);
        get_name(base, size, (int)get16(p + 32), e->name);
        e->id = get32(p + 34);
        e->data_len = get32(p + 38);
        e->rsrc_len = get32(p + 42);
    }
}

int32_t afp_enumerate(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int start,
                      afp_entry *out, int max, int *count)
{
    int32_t err;
    int n = 0, len, i, p, act;

    *count = 0;
    cmd[n++] = FP_ENUMERATE;
    cmd[n++] = 0;
    n += put16(cmd + n, vol_id);
    n += put32(cmd + n, dir_id);
    n += put16(cmd + n, P_CDATE | P_MDATE | F_FINDER | F_LONGNAME | F_FILENUM | F_DATALEN | F_RSRCLEN);
    n += put16(cmd + n, P_CDATE | P_MDATE | D_LONGNAME | D_DIRID | D_OFFSPRING);
    n += put16(cmd + n, (unsigned)max);         /* ReqCount */
    n += put16(cmd + n, (unsigned)start);       /* StartIndex, 1-based */
    n += put16(cmd + n, sizeof reply);          /* MaxReplySize */
    cmd[n++] = PATH_LONG;
    n += put_pstring(cmd + n, path);
    err = asp_command(s, cmd, n, reply, sizeof reply, &len);
    if (err)
        return err;
    if (len < 6)
        return ATK_E_FRAME;
    act = (int)get16(reply + 4);
    p = 6;
    for (i = 0; i < act && *count < max && p + 2 <= len; i++) {
        int size = reply[p];            /* includes itself and the flag byte */
        if (size < 2 || p + size > len)
            break;
        decode_entry(reply + p + 2, size - 2, (reply[p + 1] & 0x80) != 0, &out[*count]);
        (*count)++;
        p += size;
    }
    return 0;
}

static int32_t open_fork(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int rsrc,
                         unsigned access, uint16_t *fork)
{
    int32_t err;
    int n = 0, len;

    cmd[n++] = FP_OPEN_FORK;
    cmd[n++] = rsrc ? 0x80 : 0x00;
    n += put16(cmd + n, vol_id);
    n += put32(cmd + n, dir_id);
    n += put16(cmd + n, 0);             /* no file parameters */
    n += put16(cmd + n, access);        /* access mode: 1 read, 2 write */
    cmd[n++] = PATH_LONG;
    n += put_pstring(cmd + n, path);
    err = asp_command(s, cmd, n, reply, sizeof reply, &len);
    if (err)
        return err;
    if (len < 4)
        return ATK_E_FRAME;
    *fork = (uint16_t)get16(reply + 2);
    return 0;
}

int32_t afp_open_fork(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int rsrc,
                      uint16_t *fork)
{
    return open_fork(s, vol_id, dir_id, path, rsrc, 0x0001, fork);
}

int32_t afp_open_fork_write(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int rsrc,
                            uint16_t *fork)
{
    return open_fork(s, vol_id, dir_id, path, rsrc, 0x0003, fork);
}

int32_t afp_create_file(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path)
{
    int n = 0, len;

    cmd[n++] = FP_CREATE_FILE;
    cmd[n++] = 0x00;                    /* a soft create: fail if the file exists */
    n += put16(cmd + n, vol_id);
    n += put32(cmd + n, dir_id);
    cmd[n++] = PATH_LONG;
    n += put_pstring(cmd + n, path);
    return asp_command(s, cmd, n, reply, sizeof reply, &len);
}

int32_t afp_create_dir(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, uint32_t *new_id)
{
    int32_t err;
    int n = 0, len;

    cmd[n++] = FP_CREATE_DIR;
    cmd[n++] = 0;
    n += put16(cmd + n, vol_id);
    n += put32(cmd + n, dir_id);
    cmd[n++] = PATH_LONG;
    n += put_pstring(cmd + n, path);
    err = asp_command(s, cmd, n, reply, sizeof reply, &len);
    if (!err && len >= 4)
        *new_id = get32(reply);
    return err;
}

int32_t afp_set_finder_info(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path,
                            const uint8_t finder[32])
{
    int n = 0, len;

    cmd[n++] = FP_SET_FILE_PARMS;
    cmd[n++] = 0;
    n += put16(cmd + n, vol_id);
    n += put32(cmd + n, dir_id);
    n += put16(cmd + n, F_FINDER);
    cmd[n++] = PATH_LONG;
    n += put_pstring(cmd + n, path);
    if (n & 1)                          /* the parameters start on an even byte */
        cmd[n++] = 0;
    memcpy(cmd + n, finder, 32);
    n += 32;
    return asp_command(s, cmd, n, reply, sizeof reply, &len);
}

int32_t afp_write(asp_session *s, uint16_t fork, uint32_t offset, const uint8_t *data, int len)
{
    int n = 0, rlen;

    if (len > 8 * ATP_MAX_DATA)
        len = 8 * ATP_MAX_DATA;
    cmd[n++] = FP_WRITE;
    cmd[n++] = 0;                       /* the offset is from the start of the fork */
    n += put16(cmd + n, fork);
    n += put32(cmd + n, offset);
    n += put32(cmd + n, (uint32_t)len);
    return asp_write(s, cmd, n, data, len, reply, sizeof reply, &rlen);
}

int32_t afp_read(asp_session *s, uint16_t fork, uint32_t offset, int count, uint8_t *buf, int *got)
{
    int32_t err;
    int n = 0, len;

    *got = 0;
    if (count > (int)sizeof reply)
        count = (int)sizeof reply;
    cmd[n++] = FP_READ;
    cmd[n++] = 0;
    n += put16(cmd + n, fork);
    n += put32(cmd + n, offset);
    n += put32(cmd + n, (uint32_t)count);
    cmd[n++] = 0;                       /* no newline checking */
    cmd[n++] = 0;
    err = asp_command(s, cmd, n, reply, sizeof reply, &len);
    if (len > count)
        len = count;
    memcpy(buf, reply, (size_t)len);
    *got = len;
    return err;                         /* EOFErr comes with the last data */
}

int32_t afp_close_fork(asp_session *s, uint16_t fork)
{
    int len;

    cmd[0] = FP_CLOSE_FORK;
    cmd[1] = 0;
    put16(cmd + 2, fork);
    return asp_command(s, cmd, 4, reply, sizeof reply, &len);
}
