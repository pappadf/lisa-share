/* Lisa Share: an Office System desktop tool that browses AppleShare servers
 * over LocalTalk and copies files between them and the Lisa desktop.
 *
 *   main.c     startup, events, menus, the Filer protocol, documents
 *   browser.c  the browser: servers, volumes, folders (lib/ for the network)
 *   view.c     its window, drawn like a Desktop Manager disk or folder
 *   macfile.c  Mac file documents: a file copied as it is, its forks and
 *              Finder info kept under the document's file prefix */
#ifndef LISASHARE_H
#define LISASHARE_H
#include <stdint.h>
#include "lisa.h"
#include "lisa_os.h"
#include "lisa/desktop.h"
#include "atk.h"

#define TOOL_ID 22                      /* {T22}: the tool's number */

/* menus (lisashare.phrase) */
enum { M_FILE = 1, M_EDIT, M_SERVER, M_HOUSE, N_MENUS = 4 };
enum { F_SETASIDEALL = 1, F_SETASIDE };
enum { E_COPY = 1, E_COPYLW, E_PASTE = 4, E_PASTETEXT };
enum { S_FIND = 1, S_OPEN, S_UP, S_DISCONNECT = 5 };
enum { V_PICTORIAL = 1, V_ALPHA, V_CHRONO };    /* Housekeeping: the views */

/* main.c */
void pstr(unsigned char *p, const char *s);
void cstr(char *out, const unsigned char *p, int max);
void menus_update(void);
void status(const char *fmt, ...);
extern char status_line[90];

/* browser.c */
typedef struct view_row {
    int kind;                           /* the Desktop Manager's icon: computerKind, diskKind, ... */
    char name[33];
    int32_t size;                       /* in blocks, or -1 */
    int has_dates;
    int32_t mdate, cdate;               /* seconds from 1970 (server time), 0 for none */
} view_row;
int browser_nrows(void);
void browser_row(int i, view_row *r);
int browser_sel(void);                  /* the selected row, or -1 */
void browser_set_sel(int i);
void browser_open_sel(void);            /* a double click */
void browser_where(char *s, int size);  /* "server > volume > folder" */
void browser_command(int item);         /* the Server menu */
int browser_can(int item);              /* is the Server menu item usable? */
int browser_can_copy(void);
int browser_can_paste(void);
void browser_paste(void);               /* Edit > Paste: upload the Clipboard's documents */
int browser_can_copy_lw(void);
void browser_copy_lw(WindowPtr w);      /* Edit > Copy as LisaWrite: the selected server file */
void browser_copy_macfile_lw(WindowPtr w, const char *prefix);  /* ... a Mac file document */
void browser_paste_text(void);          /* Edit > Paste as Text: LisaWrite documents as text files */
void browser_copy(WindowPtr w);         /* Edit > Copy: a reference on the Clipboard */
void browser_idle(void);
void browser_shutdown(void);
/* Copy the file a Clipboard reference names ("-LS-<n>") into a new
   document at `prefix`.  0 or an OS/AFP error. */
int32_t browser_download(const char *source, const char *prefix);

/* view.c */
void view_init(void);
int view_mode(void);
void view_set_mode(WindowPtr w, int mode);
void view_changed(void);                /* the rows changed: scroll back to the top */
void view_draw(WindowPtr w, int active);
void view_activate(WindowPtr w, int on);
void view_click(WindowPtr w, Point where, int32_t when);

/* macfile.c */
typedef struct macfile_info {
    char name[33];
    uint8_t finder[32];                 /* type at 0, creator at 4 */
    uint32_t data_len, rsrc_len;
    char server[33], volume[28];
} macfile_info;

int macfile_is(const char *prefix);     /* does a Mac file document live at prefix? */
int macfile_read_info(const char *prefix, macfile_info *mi);
int macfile_open_fork(const char *prefix, int rsrc, int16_t *refnum);   /* to read; 0 or an error */
int32_t macfile_read(int16_t refnum, void *buf, int32_t len);          /* bytes read, 0 at the end */
int macfile_write_info(const char *prefix, const macfile_info *mi);
int macfile_create_fork(const char *prefix, int rsrc, int16_t *refnum);
int macfile_write(int16_t refnum, const void *data, int32_t len);
void macfile_close(int16_t refnum);
int macfile_copy(const char *from, const char *to);     /* a document's files to a new prefix */
void macfile_draw(WindowPtr w, const char *prefix);

/* lisawrite.c: LisaWrite documents (tool 1), one file named the document's prefix */
#define LW_TOOL 1
#define LW_MAX_TEXT 60000               /* the longest text converted */
int lw_write(const char *path, const uint8_t *text, int32_t len);  /* 0 or an error */
int32_t lw_read(const char *path, uint8_t *out, int32_t max);      /* the body's length, or -1 */

/* lisadoc.c: any Lisa document's files (for copying it as it is) */
int lisadoc_tool(const char *prefix);   /* the tool number in -vol-{DnTm} */
int lisadoc_list(const char *prefix, char names[][33], int max);    /* its files' names */
int lisadoc_read_label(const char *path, uint8_t *label, int max);  /* label length */
int lisadoc_create(const char *path, const uint8_t *label, int label_len, int16_t *refnum);
int lisadoc_open(const char *path, int16_t *refnum);                /* to read */

#endif
