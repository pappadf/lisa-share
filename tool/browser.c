/* The network browser: servers, then a server's volumes, then folders.
 *
 * One row per item (view.c draws them); a click selects a row, a double
 * click (or Server > Open) opens it: a server is connected to and logged in as a guest, a volume is
 * opened, a folder is listed.  Server > Go Up goes back one level.  Edit >
 * Copy puts a reference to the selected file on the Clipboard; pasting it on
 * the desktop makes the Desktop Manager ask us for the document
 * (browser_download).  The network calls are lib/atk.h's; they poll, so the
 * tool does nothing else while one runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lisashare.h"

enum { SERVERS, VOLUMES, FOLDER };
#define MAXROWS 40
#define MAXDEPTH 8
#define MAXREFS 16

static struct {
    int net;                            /* the driver is installed */
    int level;
    nbp_entity servers[8];
    int nservers, server;
    asp_session s;
    char vols[8][28];
    int nvols;
    char vol[28];
    uint16_t vol_id;
    uint32_t dirs[MAXDEPTH + 1];        /* dirs[0] = the volume's root */
    char names[MAXDEPTH + 1][33];
    int depth;
    afp_entry items[MAXROWS];
    int nitems;
    int sel;                            /* the selected row, or -1 */
    /* the files Edit > Copy referred to, for browser_download */
    struct ref {
        char kind;                      /* S Mac file, B Lisa document, W server file as LisaWrite,
                                           M Mac file document as LisaWrite */
        char server[33], vol[28], name[33];
        uint32_t dir;
        afp_entry e;
        char local[64];                 /* M: the Mac file document's prefix */
    } refs[MAXREFS];
    int nrefs;
} br = {.sel = -1};

static int is_bundle(const afp_entry *e);

static int rows(void)
{
    return br.level == SERVERS ? br.nservers : br.level == VOLUMES ? br.nvols : br.nitems;
}

static int ensure_net(void)
{
    int err;

    if (br.net)
        return 0;
    status("Installing the LocalTalk driver on Serial B...");
    err = atk_open();
    if (err) {
        status("Could not use Serial B (%s, error %d).", atk_failed_step(), err);
        return err;
    }
    br.net = 1;
    return 0;
}

static void find_servers(void)
{
    if (ensure_net())
        return;
    if (br.s.open) {
        afp_logout(&br.s);
        asp_close(&br.s);
    }
    br.level = SERVERS;
    br.sel = -1;
    view_changed();
    br.nservers = nbp_lookup("=", "AFPServer", "*", br.servers, 8, 150, 3);
    status("%d file server%s found.", br.nservers, br.nservers == 1 ? "" : "s");
}

/* Did the call fail because the session is gone?  The server closes a
   session that has been silent for two minutes, which happens whenever the
   tool's window stays inactive that long (an inactive tool does not run, so
   it cannot tickle). */
static int session_lost(int32_t err)
{
    return err == AFP_SESS_CLOSED || err == ATP_E_TIMEOUT || (err && !br.s.open);
}

/* Open a new session to the same server, log in again and reopen the volume
   (directory IDs stay valid: AFP 2 volumes have fixed ones). */
static int32_t reconnect(void)
{
    nbp_entity *srv = &br.servers[br.server];
    int32_t err;

    br.s.open = 0;
    err = asp_open(&br.s, srv->node, srv->socket);
    if (!err)
        err = afp_login(&br.s);
    if (!err && br.level == FOLDER)
        err = afp_open_vol(&br.s, br.vol, &br.vol_id);
    return err;
}

static int32_t enumerate(void)
{
    return afp_enumerate(&br.s, br.vol_id, br.dirs[br.depth], "", 1, br.items, MAXROWS, &br.nitems);
}

static int list_folder(void)
{
    int32_t err;

    err = enumerate();
    if (session_lost(err) && reconnect() == 0)
        err = enumerate();
    if (err == AFP_OBJECT_NOT_FOUND) {  /* an empty folder */
        br.nitems = 0;
        err = 0;
    }
    br.sel = -1;
    view_changed();
    if (err)
        status("Could not list the folder (error %ld).", (long)err);
    else
        status("%d item%s.", br.nitems, br.nitems == 1 ? "" : "s");
    return (int)err;
}

static void open_server(int i)
{
    int32_t err;

    status("Connecting to %s...", br.servers[i].object);
    err = asp_open(&br.s, br.servers[i].node, br.servers[i].socket);
    if (!err)
        err = afp_login(&br.s);
    if (!err)
        err = afp_volumes(&br.s, br.vols, 8, &br.nvols);
    if (err) {
        status("Could not connect to %s (error %ld).", br.servers[i].object, (long)err);
        asp_close(&br.s);
        return;
    }
    br.server = i;
    br.level = VOLUMES;
    br.sel = -1;
    view_changed();
    status("Connected to %s as a guest.", br.servers[i].object);
}

static void open_volume(int i)
{
    int32_t err = afp_open_vol(&br.s, br.vols[i], &br.vol_id);

    if (err) {
        status("Could not open %s (error %ld).", br.vols[i], (long)err);
        return;
    }
    snprintf(br.vol, sizeof br.vol, "%s", br.vols[i]);
    br.depth = 0;
    br.dirs[0] = AFP_ROOT;
    snprintf(br.names[0], sizeof br.names[0], "%s", br.vol);
    br.level = FOLDER;
    list_folder();
}

static void open_row(int i)
{
    if (i < 0 || i >= rows())
        return;
    if (br.level == SERVERS) {
        open_server(i);
    } else if (br.level == VOLUMES) {
        open_volume(i);
    } else if (br.items[i].is_dir && !is_bundle(&br.items[i]) && br.depth < MAXDEPTH) {
        br.depth++;
        br.dirs[br.depth] = br.items[i].id;
        snprintf(br.names[br.depth], sizeof br.names[0], "%s", br.items[i].name);
        list_folder();
    }
}

static void go_up(void)
{
    if (br.level == FOLDER && br.depth > 0) {
        br.depth--;
        list_folder();
    } else if (br.level == FOLDER) {
        afp_close_vol(&br.s, br.vol_id);
        br.level = VOLUMES;
        br.sel = -1;
    view_changed();
        status("Volumes on %s.", br.servers[br.server].object);
    } else if (br.level == VOLUMES) {
        afp_logout(&br.s);
        asp_close(&br.s);
        br.level = SERVERS;
        br.sel = -1;
    view_changed();
        status("Disconnected.");
    }
}

int browser_can(int item)
{
    switch (item) {
    case S_FIND:       return 1;
    case S_OPEN:       return br.sel >= 0 && (br.level != FOLDER ||
                                              (br.items[br.sel].is_dir && !is_bundle(&br.items[br.sel])));
    case S_UP:         return br.level != SERVERS;
    case S_DISCONNECT: return br.level != SERVERS;
    }
    return 0;
}

void browser_command(int item)
{
    switch (item) {
    case S_FIND:
        find_servers();
        break;
    case S_OPEN:
        open_row(br.sel);
        break;
    case S_UP:
        go_up();
        break;
    case S_DISCONNECT:
        while (br.level != SERVERS)
            go_up();
        break;
    }
}

/* A folder named "<document>.lisa" holds a Lisa document copied as it is:
   its files, and a MANIFEST naming the document, its tool and the files with
   their labels (tab-separated lines: "file", the file's suffix after the
   {DnTm} prefix, its name on the server, its label in hex). */
#define BUNDLE ".lisa"
#define MAXFILES 24

typedef struct manifest {
    int tool;
    char name[81];
    int nfiles;
    struct mfile {
        char suffix[33], srv[33];
        int label_len;
        uint8_t label[128];
    } f[MAXFILES];
} manifest;

/* Buffers, shared to keep the tool's globals small: every one of them is
   memory a 1 MB Lisa must find for the tool's data segment before it starts.
   xfer is for one file copy at a time (fetch_fork, send_file) or a MANIFEST
   being read; the manifest read (man) and the one being written (mtext) are
   never needed together. */
static uint8_t xfer[4096];
static union {
    manifest m;
    char text[4096];
} mbuf;
#define man (mbuf.m)
#define mtext (mbuf.text)

static int is_bundle(const afp_entry *e)
{
    size_t n = strlen(e->name);

    return e->is_dir && n > 5 && strcmp(e->name + n - 5, BUNDLE) == 0;
}

int browser_can_copy(void)
{
    return br.level == FOLDER && br.sel >= 0 && (!br.items[br.sel].is_dir || is_bundle(&br.items[br.sel]));
}

/* Read the open fork of a server file to its end into a local file. */
static int32_t fetch_fork(uint16_t fork, int16_t refnum, uint32_t *total)
{
    int32_t err;
    int got, e;

    *total = 0;
    for (;;) {
        err = afp_read(&br.s, fork, *total, sizeof xfer, xfer, &got);
        if (got > 0 && (e = macfile_write(refnum, xfer, got)) != 0)
            return e;
        *total += (uint32_t)got;
        if (err == AFP_EOF || got == 0)
            return 0;
        if (err)
            return err;
    }
}

/* Copy one fork of a server file into a local file. */
static int32_t fetch(uint16_t vol, uint32_t dir, const char *name, int rsrc, int16_t refnum, uint32_t *total)
{
    uint16_t fork;
    int32_t err = afp_open_fork(&br.s, vol, dir, name, rsrc, &fork);

    *total = 0;
    if (err)
        return err;
    err = fetch_fork(fork, refnum, total);
    afp_close_fork(&br.s, fork);
    return err;
}

/* Send a local file into one fork of a new file on the server. */
static int32_t send_file(const char *path, uint32_t dir, const char *name, int rsrc, uint32_t *total)
{
    int16_t refnum;
    uint16_t fork;
    int32_t err, got;

    *total = 0;
    if (lisadoc_open(path, &refnum))
        return 0;                       /* no such file: nothing to send */
    err = afp_open_fork_write(&br.s, br.vol_id, dir, name, rsrc, &fork);
    if (err) {
        macfile_close(refnum);
        return err;
    }
    while (!err && (got = macfile_read(refnum, xfer, sizeof xfer)) > 0) {
        err = afp_write(&br.s, fork, *total, xfer, (int)got);
        *total += (uint32_t)got;
    }
    afp_close_fork(&br.s, fork);
    macfile_close(refnum);
    return err;
}

/* Parse a MANIFEST (text) into man. */
static int parse_manifest(char *text)
{
    char *line, *next, *f[5];
    int i, nf;

    memset(&man, 0, sizeof man);
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        for (nf = 0, f[0] = line; nf < 4 && f[nf]; nf++) {
            char *t = strchr(f[nf], '\t');
            if (t)
                *t++ = 0;
            f[nf + 1] = t;
        }
        if (strcmp(f[0], "tool") == 0 && nf > 1) {
            man.tool = 0;
            sscanf(f[1], "%d", &man.tool);
        } else if (strcmp(f[0], "name") == 0 && nf > 1) {
            snprintf(man.name, sizeof man.name, "%s", f[1]);
        } else if (strcmp(f[0], "file") == 0 && nf > 3 && man.nfiles < MAXFILES) {
            struct mfile *m = &man.f[man.nfiles++];
            snprintf(m->suffix, sizeof m->suffix, "%s", f[1]);
            snprintf(m->srv, sizeof m->srv, "%s", f[2]);
            for (i = 0; f[3][2 * i] && f[3][2 * i + 1] && i < 128; i++) {
                unsigned v;
                sscanf(f[3] + 2 * i, "%2x", &v);
                m->label[i] = (uint8_t)v;
            }
            m->label_len = i;
        }
    }
    return man.tool > 0 && man.nfiles > 0 ? 0 : 1;
}

static int32_t read_manifest(uint16_t vol, uint32_t dir)
{
    char *text = (char *)xfer;
    uint16_t fork;
    int32_t err;
    int got;

    err = afp_open_fork(&br.s, vol, dir, "MANIFEST", 0, &fork);
    if (err)
        return err;
    err = afp_read(&br.s, fork, 0, sizeof xfer - 1, xfer, &got);
    afp_close_fork(&br.s, fork);
    if (err && err != AFP_EOF)
        return err;
    text[got > 0 ? got : 0] = 0;
    return parse_manifest(text) ? ATK_E_FRAME : 0;
}

/* Put a reference on the Clipboard to a document of `tool` named `name`,
   which we make when it is pasted (browser_download); its private name is
   "-L<kind>-<n>". */
static struct ref *put_ref(WindowPtr w, char kind, int tool, const char *name)
{
    struct ref *r;
    TIconRef ref;
    Integer err;
    char disk[16];
    int n = br.nrefs++ % MAXREFS;

    r = &br.refs[n];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    memset(&ref, 0, sizeof ref);
    ref.kind = docKind;
    ref.toolID = tool;
    pstr(ref.userName, name);
    snprintf(disk, sizeof disk, "-L%c-%d", kind, n);
    pstr(ref.diskName, disk);
    StartIconRef(&err, w);
    if (err > 0) {
        status("Could not use the Clipboard (error %d).", err);
        return 0;
    }
    AddIconRef(&ref);
    EndIconRef();
    status("\"%s\" is on the Clipboard: paste it into a disk or folder.", name);
    return r;
}

/* The selected server file or ".lisa" folder, for put_ref */
static void remember_selected(struct ref *r, int bundle)
{
    afp_entry *e = &br.items[br.sel];

    snprintf(r->server, sizeof r->server, "%s", br.servers[br.server].object);
    snprintf(r->vol, sizeof r->vol, "%s", br.vol);
    snprintf(r->name, sizeof r->name, "%s", e->name);
    r->dir = bundle ? e->id : br.dirs[br.depth];
    r->e = *e;
}

void browser_copy(WindowPtr w)
{
    afp_entry *e;
    struct ref *r;

    if (!browser_can_copy())
        return;
    e = &br.items[br.sel];
    if (is_bundle(e)) {                 /* a Lisa document: it goes back to its own tool */
        if (read_manifest(br.vol_id, e->id)) {
            status("\"%s\" has no readable MANIFEST.", e->name);
            return;
        }
        if ((r = put_ref(w, 'B', man.tool, man.name)) != 0)
            remember_selected(r, 1);
    } else if ((r = put_ref(w, 'S', TOOL_ID, e->name)) != 0) {     /* a Mac file */
        remember_selected(r, 0);
    }
}

int browser_can_copy_lw(void)
{
    return br.level == FOLDER && br.sel >= 0 && !br.items[br.sel].is_dir;
}

/* The document name for a converted file: without a ".txt" ending. */
static void doc_name(const char *file, char *name)
{
    size_t n = strlen(file);

    snprintf(name, 33, "%s", file);
    if (n > 4 && (strcmp(file + n - 4, ".txt") == 0 || strcmp(file + n - 4, ".TXT") == 0))
        name[n - 4] = 0;
}

void browser_copy_lw(WindowPtr w)
{
    struct ref *r;
    char name[33];

    if (!browser_can_copy_lw())
        return;
    doc_name(br.items[br.sel].name, name);
    if ((r = put_ref(w, 'W', LW_TOOL, name)) != 0)
        remember_selected(r, 0);
}

void browser_copy_macfile_lw(WindowPtr w, const char *prefix)
{
    macfile_info mi;
    struct ref *r;
    char name[33];

    if (macfile_read_info(prefix, &mi))
        return;
    doc_name(mi.name, name);
    if ((r = put_ref(w, 'M', LW_TOOL, name)) != 0)
        snprintf(r->local, sizeof r->local, "%s", prefix);
}

/* The text being converted.  Taken from the heap only while converting
   (malloc's data segment): the tool must fit in a 1 MB Lisa. */
static uint8_t *text;

static int text_alloc(void)
{
    if (!text)
        text = malloc(LW_MAX_TEXT + 1);   /* + a final CR (lw_write) */
    return text != 0;
}

static void text_free(void)
{
    free(text);
    text = 0;
}

/* A server file's data fork into `text`; its length, or -1. */
static int32_t fetch_text(uint16_t vol, struct ref *r)
{
    uint16_t fork;
    int32_t err, n = 0;
    int got;

    if (afp_open_fork(&br.s, vol, r->dir, r->name, 0, &fork))
        return -1;
    do {
        err = afp_read(&br.s, fork, (uint32_t)n, (int)(LW_MAX_TEXT - n > 4096 ? 4096 : LW_MAX_TEXT - n),
                       text + n, &got);
        n += got;
    } while (!err && got > 0 && n < LW_MAX_TEXT);
    afp_close_fork(&br.s, fork);
    return (err && err != AFP_EOF) ? -1 : n;
}

/* A Mac file document's data fork into `text` */
static int32_t local_text(const char *prefix)
{
    int16_t refnum;
    int32_t n = 0, got;

    if (macfile_open_fork(prefix, 0, &refnum))
        return -1;
    while (n < LW_MAX_TEXT && (got = macfile_read(refnum, text + n, LW_MAX_TEXT - n)) > 0)
        n += got;
    macfile_close(refnum);
    return n;
}

/* A Mac file into a new Mac file document. */
static int32_t download_macfile(struct ref *r, uint16_t vol, const char *prefix)
{
    macfile_info mi;
    int16_t refnum;
    int32_t err;

    memset(&mi, 0, sizeof mi);
    snprintf(mi.name, sizeof mi.name, "%s", r->name);
    memcpy(mi.finder, r->e.finder, 32);
    snprintf(mi.server, sizeof mi.server, "%s", r->server);
    snprintf(mi.volume, sizeof mi.volume, "%s", r->vol);
    if ((err = macfile_create_fork(prefix, 0, &refnum)) != 0)
        return err;
    err = fetch(vol, r->dir, r->name, 0, refnum, &mi.data_len);
    macfile_close(refnum);
    if (!err && r->e.rsrc_len > 0) {
        if ((err = macfile_create_fork(prefix, 1, &refnum)) != 0)
            return err;
        err = fetch(vol, r->dir, r->name, 1, refnum, &mi.rsrc_len);
        macfile_close(refnum);
    }
    if (!err)
        err = macfile_write_info(prefix, &mi);
    if (!err)
        status("Copied \"%s\": %lu + %lu bytes.", r->name, (unsigned long)mi.data_len,
               (unsigned long)mi.rsrc_len);
    return err;
}

/* A ".lisa" folder back into the Lisa document it holds, file by file with
   the files' labels. */
static int32_t download_bundle(struct ref *r, uint16_t vol, const char *prefix)
{
    char path[256];
    int16_t refnum;
    uint32_t len;
    int32_t err;
    int i;

    if ((err = read_manifest(vol, r->dir)) != 0)
        return err;
    for (i = 0; i < man.nfiles && !err; i++) {
        snprintf(path, sizeof path, "%s%s", prefix, man.f[i].suffix);
        if ((err = lisadoc_create(path, man.f[i].label, man.f[i].label_len, &refnum)) != 0)
            break;
        err = fetch(vol, r->dir, man.f[i].srv, 0, refnum, &len);
        macfile_close(refnum);
    }
    if (!err)
        status("Copied \"%s\" (%d files).", man.name, man.nfiles);
    return err;
}

int32_t browser_download(const char *source, const char *prefix)
{
    struct ref *r;
    uint16_t vol;
    int32_t err;
    int n = 0;

    sscanf(source + 4, "%d", &n);
    if (n < 0 || n >= br.nrefs || n < br.nrefs - MAXREFS)
        return ATK_E_FUNCTION;
    r = &br.refs[n % MAXREFS];
    if ((r->kind == 'M' || r->kind == 'W') && !text_alloc()) {
        status("There is not enough memory to convert \"%s\".", r->name);
        return ATK_E_FRAME;
    }
    if (r->kind == 'M') {               /* a Mac file document, converted: no network */
        int32_t len = local_text(r->local);
        err = len < 0 ? ATK_E_FRAME : lw_write(prefix, text, len);
        text_free();
        if (!err)
            status("Converted to LisaWrite: %ld characters.", (long)len);
        return err;
    }
    if (br.level == SERVERS)
        return ATK_E_FUNCTION;          /* disconnected since the copy */
    status("Copying \"%s\"...", r->name);
    err = afp_open_vol(&br.s, r->vol, &vol);
    if (session_lost(err) && reconnect() == 0)
        err = afp_open_vol(&br.s, r->vol, &vol);
    if (!err && r->kind == 'W') {      /* a server file as a LisaWrite document */
        int32_t len = fetch_text(vol, r);
        err = len < 0 ? ATK_E_FRAME : lw_write(prefix, text, len);
        text_free();
        if (!err)
            status("Copied \"%s\" as a LisaWrite document: %ld characters.", r->name, (long)len);
    } else if (!err) {
        err = r->kind == 'B' ? download_bundle(r, vol, prefix) : download_macfile(r, vol, prefix);
    }
    if (err)
        status("Could not copy \"%s\" (error %ld).", r->name, (long)err);
    return err;
}

/* ---- Edit > Paste: documents from the desktop to the server ---------------- */

/* ReadIconRefs calls collect_ref once per icon on the Clipboard, with the
   reference in these globals. */
static ProcessId ref_proc;
static WindowPtr ref_window;
static TIconRef ref_identity;
static TIconRef pasted[8];
static int npasted;

static pascal void collect_ref(void)
{
    if (npasted < 8)
        pasted[npasted++] = ref_identity;
}

int browser_can_paste(void)
{
    return br.level == FOLDER;
}

/* Make a file (or, `dir`, a folder) in the current folder under `want`,
   or "want 2", "want 3", ... if the name is taken; the name used goes to
   `name`.  For a folder its ID goes to *id. */
static int32_t create_unique(const char *want, int dir, char *name, uint32_t *id)
{
    int32_t err;
    int k;
    size_t n = strlen(want), ext = dir ? strlen(BUNDLE) : 0;

    for (k = 1; k < 10; k++) {
        if (k == 1)
            snprintf(name, 33, "%s", want);
        else if (dir)                   /* "doc 2.lisa" */
            snprintf(name, 33, "%.*s %d%s", (int)(n - ext > 24 ? 24 : n - ext), want, k, BUNDLE);
        else
            snprintf(name, 33, "%.29s %d", want, k);
        err = dir ? afp_create_dir(&br.s, br.vol_id, br.dirs[br.depth], name, id)
                  : afp_create_file(&br.s, br.vol_id, br.dirs[br.depth], name);
        if (session_lost(err) && reconnect() == 0)
            err = dir ? afp_create_dir(&br.s, br.vol_id, br.dirs[br.depth], name, id)
                      : afp_create_file(&br.s, br.vol_id, br.dirs[br.depth], name);
        if (err != AFP_OBJECT_EXISTS)
            return err;
    }
    return AFP_OBJECT_EXISTS;
}

/* A Mac file document: the file it was, under its own name. */
static void upload_macfile(const char *prefix)
{
    macfile_info mi;
    char name[33], path[256];
    uint32_t dlen = 0, rlen = 0, id;
    int32_t err;

    if (macfile_read_info(prefix, &mi)) {
        status("The document's information cannot be read.");
        return;
    }
    err = create_unique(mi.name, 0, name, &id);
    if (!err && mi.finder[0])
        err = afp_set_finder_info(&br.s, br.vol_id, br.dirs[br.depth], name, mi.finder);
    snprintf(path, sizeof path, "%sDATA", prefix);
    if (!err)
        err = send_file(path, br.dirs[br.depth], name, 0, &dlen);
    snprintf(path, sizeof path, "%sRSRC", prefix);
    if (!err)
        err = send_file(path, br.dirs[br.depth], name, 1, &rlen);
    if (err)
        status("Could not copy \"%s\" to the server (error %ld).", mi.name, (long)err);
    else
        status("Copied \"%s\" to the server: %lu + %lu bytes.", name, (unsigned long)dlen, (unsigned long)rlen);
}

/* Any other Lisa document: a ".lisa" folder with its files and a MANIFEST. */
static void upload_lisadoc(const char *prefix, const char *docname)
{
    static char names[MAXFILES][33];
    char *text = mtext;
    char folder[33], want[40], path[256], srv[33], vol[64];
    const char *base = strrchr(prefix, '-');
    uint32_t dir, len;
    uint8_t label[128];
    int32_t err;
    int i, j, n, nfiles, label_len, base_len;

    nfiles = lisadoc_list(prefix, names, MAXFILES);
    if (!base || nfiles == 0) {
        status("\"%s\" has no files to copy.", docname);
        return;
    }
    snprintf(vol, sizeof vol, "%.*s", (int)(base - prefix), prefix);
    base_len = (int)strlen(base + 1);
    snprintf(want, sizeof want, "%.27s%s", docname, BUNDLE);
    err = create_unique(want, 1, folder, &dir);
    n = snprintf(text, sizeof mtext, "LisaShare document 1\ntool\t%d\nname\t%s\n", lisadoc_tool(prefix), docname);
    for (i = 0; i < nfiles && !err; i++) {
        const char *suffix = names[i] + base_len;
        snprintf(srv, sizeof srv, "%s", suffix[0] ? suffix : "(label)");
        snprintf(path, sizeof path, "%s-%s", vol, names[i]);
        label_len = lisadoc_read_label(path, label, sizeof label);
        err = afp_create_file(&br.s, br.vol_id, dir, srv);
        if (!err)
            err = send_file(path, dir, srv, 0, &len);
        n += snprintf(text + n, sizeof mtext - (size_t)n, "file\t%s\t%s\t", suffix, srv);
        for (j = 0; j < label_len && n < (int)sizeof mtext - 3; j++)
            n += snprintf(text + n, sizeof mtext - (size_t)n, "%02x", label[j]);
        n += snprintf(text + n, sizeof mtext - (size_t)n, "\n");
    }
    if (!err)
        err = afp_create_file(&br.s, br.vol_id, dir, "MANIFEST");
    if (!err) {
        uint16_t fork;
        err = afp_open_fork_write(&br.s, br.vol_id, dir, "MANIFEST", 0, &fork);
        if (!err) {
            err = afp_write(&br.s, fork, 0, (const uint8_t *)text, n);
            afp_close_fork(&br.s, fork);
        }
    }
    if (err)
        status("Could not copy \"%s\" to the server (error %ld).", docname, (long)err);
    else
        status("Copied \"%s\" to the server as \"%s\" (%d files).", docname, folder, nfiles);
}

/* Edit > Paste as Text: each LisaWrite document on the Clipboard becomes a
   text file (type TEXT, creator ttxt) of its body's text. */
void browser_paste_text(void)
{
    static const uint8_t finder[32] = {'T', 'E', 'X', 'T', 't', 't', 'x', 't'};
    char prefix[256], want[90], name[33];
    uint32_t id;
    uint16_t fork;
    int32_t len, err, off;
    Integer e;
    int i, done = 0;

    if (!browser_can_paste())
        return;
    if (!text_alloc()) {
        status("There is not enough memory to convert documents.");
        return;
    }
    npasted = 0;
    ReadIconRefs(&e, &ref_proc, &ref_window, &ref_identity, (TProc)collect_ref);
    for (i = 0; i < npasted; i++) {
        if (pasted[i].toolID != LW_TOOL)
            continue;
        cstr(prefix, pasted[i].diskName, sizeof prefix);
        cstr(want, pasted[i].userName, sizeof want);
        len = lw_read(prefix, text, LW_MAX_TEXT);
        if (len < 0) {
            status("\"%s\" cannot be read as a LisaWrite document.", want);
            continue;
        }
        err = create_unique(want, 0, name, &id);
        if (!err)
            err = afp_set_finder_info(&br.s, br.vol_id, br.dirs[br.depth], name, finder);
        if (!err)
            err = afp_open_fork_write(&br.s, br.vol_id, br.dirs[br.depth], name, 0, &fork);
        for (off = 0; !err && off < len; off += 4096)
            err = afp_write(&br.s, fork, (uint32_t)off, text + off, (int)(len - off > 4096 ? 4096 : len - off));
        if (!err || fork)
            afp_close_fork(&br.s, fork);
        if (err)
            status("Could not copy \"%s\" as text (error %ld).", want, (long)err);
        else
            status("Copied \"%s\" to the server as text: %ld characters.", name, (long)len);
        done++;
    }
    text_free();
    if (!done && npasted)
        status("Paste as Text takes LisaWrite documents.");
    else if (!npasted)
        status("The Clipboard holds no documents.");
    if (done) {
        char keep[90];
        snprintf(keep, sizeof keep, "%s", status_line);
        list_folder();
        status("%s", keep);
    }
}

void browser_paste(void)
{
    char prefix[256], name[90];
    Integer err;
    int i;

    if (!browser_can_paste())
        return;
    npasted = 0;
    ReadIconRefs(&err, &ref_proc, &ref_window, &ref_identity, (TProc)collect_ref);
    if (err > 0 || npasted == 0) {
        status("The Clipboard holds no documents.");
        return;
    }
    for (i = 0; i < npasted; i++) {
        cstr(prefix, pasted[i].diskName, sizeof prefix);
        cstr(name, pasted[i].userName, sizeof name);
        if (macfile_is(prefix))
            upload_macfile(prefix);
        else
            upload_lisadoc(prefix, name);
    }
    {
        char keep[90];
        snprintf(keep, sizeof keep, "%s", status_line);  /* the copy's result, not the listing's */
        list_folder();
        status("%s", keep);
    }
}

/* ---- the rows, for view.c ------------------------------------------------- */

int browser_nrows(void)
{
    return rows();
}

/* An AFP date as seconds from 1970, as the server's clock had it.  (A Mac
   client shifts dates by the difference between the server's clock and its
   own; the Lisa's clock cannot go past 1995, so that would show decades-old
   dates.)  AFP counts from 2000, but some servers, granny-smith's among them,
   count from 1904 as the Mac does: the server's clock tells which.  Its time
   now is 0xA0000000..0xED000000 (1989..2030) from 1904, otherwise -2^28..2^30
   (1991..2034) from 2000. */
static int32_t lisa_date(int32_t afp)
{
    if (afp == (int32_t)0x80000000)     /* never */
        return 0;
    if (br.s.server_time >= 0xA0000000u && br.s.server_time < 0xED000000u)
        return (int32_t)((uint32_t)afp - 2082844800u);
    return afp + 946684800;
}

void browser_row(int i, view_row *r)
{
    memset(r, 0, sizeof *r);
    r->size = -1;
    if (br.level == SERVERS) {
        r->kind = computerKind;
        snprintf(r->name, sizeof r->name, "%s", br.servers[i].object);
    } else if (br.level == VOLUMES) {
        r->kind = diskKind;
        snprintf(r->name, sizeof r->name, "%s", br.vols[i]);
    } else {
        afp_entry *e = &br.items[i];
        r->has_dates = 1;
        r->mdate = lisa_date(e->mdate);
        r->cdate = lisa_date(e->cdate);
        if (is_bundle(e)) {             /* a Lisa document: its name without ".lisa" */
            r->kind = docKind;
            snprintf(r->name, sizeof r->name, "%.*s", (int)strlen(e->name) - 5, e->name);
        } else if (e->is_dir) {
            r->kind = folderkind;
            snprintf(r->name, sizeof r->name, "%s", e->name);
        } else {
            r->kind = docKind;
            snprintf(r->name, sizeof r->name, "%s", e->name);
            r->size = (int32_t)((e->data_len + e->rsrc_len + 511) / 512);
        }
    }
}

int browser_sel(void)
{
    return br.sel;
}

void browser_set_sel(int i)
{
    br.sel = i >= 0 && i < rows() ? i : -1;
}

void browser_open_sel(void)
{
    open_row(br.sel);
}

void browser_where(char *s, int size)
{
    int d, len;

    if (br.level == SERVERS) {
        snprintf(s, (size_t)size, "File servers on the network");
        return;
    }
    len = snprintf(s, (size_t)size, "%s", br.servers[br.server].object);
    if (br.level == FOLDER)
        for (d = 0; d <= br.depth && len < size; d++)
            len += snprintf(s + len, (size_t)(size - len), " > %s", br.names[d]);
}

void browser_idle(void)
{
    if (!br.net)
        return;
    atk_poll();                         /* the server's tickles */
    if (br.s.open && atk_time_us() - br.s.last_sent_us > 30000000u)
        asp_tickle(&br.s);
}

void browser_shutdown(void)
{
    if (!br.net)
        return;
    if (br.s.open) {
        afp_logout(&br.s);
        asp_close(&br.s);
    }
    atk_close();
    br.net = 0;
}
