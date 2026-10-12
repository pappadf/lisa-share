/* Lisa Share: the tool's startup, event loop, menus and documents.
 *
 * The startup order is lisa-c's examples/hellotool's.  The tool has two
 * kinds of document, told apart by their files:
 *   - the browser: the tool opened from its own icon (fcNone; the tool has
 *     no stationery, see the Makefile): the network browser (browser.c);
 *   - a Mac file (macfile.c): a file copied from a server as it is, made when
 *     the user pastes a reference the browser put on the Clipboard.  The
 *     Desktop Manager creates its catalog entry and sends us fcCopy with its
 *     file prefix; browser_download writes the files.
 * The network is set up on first use and taken down when the tool ends: the
 * LocalTalk driver replaces the serial driver at Serial B only while the
 * tool runs (lib/atk_link.c). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lisashare.h"

enum { NONE, BROWSER, MACFILE };
#define MAXDOCS 6

typedef struct doc {
    WindowPtr w;
    int kind;
    char prefix[64];                    /* -<volume>-{DnTm} */
} doc;

static doc docs[MAXDOCS];
static char heap[16384];
static MenuInfo menus[N_MENUS];
static TAlertFile alertFile;
static int owns_scrap;                  /* our references are on the Clipboard */
char status_line[90];

static pascal Integer HeapFull(void *zone, Integer bytesNeeded)
{
    (void)zone; (void)bytesNeeded;
    return 0;
}

static pascal void FailedAlert(void)
{
}

/* SYS_TERMINATE: give Serial B back to the serial driver, then go. */
static pascal void LastWishes(void *environment, void *data)
{
    (void)environment; (void)data;
    browser_shutdown();
    ImDying();
}

void pstr(unsigned char *p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

void cstr(char *out, const unsigned char *p, int max)
{
    int n = p[0] < max ? p[0] : max - 1;
    memcpy(out, p + 1, (size_t)n);
    out[n] = 0;
}

/* -<volume>-{T22}: the prefix of the tool's own files (its phrase file) */
static void tool_prefix(unsigned char prefix[64])
{
    static unsigned char info[600];     /* procinfoRec: progpathname first */
    Integer err;
    int n;

    lisa_info_process(&err, lisa_my_id(), info);
    n = info[0] >= 3 ? info[0] - 3 : 0;     /* without "obj" */
    if (n > 62)
        n = 62;
    memcpy(prefix + 1, info + 1, (size_t)n);
    prefix[0] = (unsigned char)n;
}

static doc *find_doc(WindowPtr w)
{
    int i;

    for (i = 0; i < MAXDOCS; i++)
        if (docs[i].kind != NONE && docs[i].w == w)
            return &docs[i];
    return 0;
}

static doc *active_doc(void)
{
    return find_doc(activeFolder);
}

static void draw(doc *d)
{
    Rect r;

    SetPort(d->w);
    if (d->kind == BROWSER) {
        view_draw(d->w, d->w == activeFolder);
        return;
    }
    r = d->w->portRect;
    EraseRect(&r);
    macfile_draw(d->w, d->prefix);
}

/* Redraw every browser window (after the browser changed). */
static void redraw_browsers(void)
{
    int i;

    for (i = 0; i < MAXDOCS; i++)
        if (docs[i].kind == BROWSER)
            draw(&docs[i]);
}

void status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status_line, sizeof status_line, fmt, ap);
    va_end(ap);
}

static void enable(int menu, int item, int on)
{
    if (on)
        EnableItem(&menus[menu - 1], item);
    else
        DisableItem(&menus[menu - 1], item);
}

void menus_update(void)
{
    doc *d = active_doc();
    int browser = d && d->kind == BROWSER, i;

    enable(M_EDIT, E_COPY, browser && browser_can_copy());
    enable(M_EDIT, E_COPYLW, (browser && browser_can_copy_lw()) || (d && d->kind == MACFILE));
    enable(M_EDIT, E_PASTE, browser && browser_can_paste());
    enable(M_EDIT, E_PASTETEXT, browser && browser_can_paste());
    enable(M_SERVER, S_FIND, browser && browser_can(S_FIND));
    enable(M_SERVER, S_OPEN, browser && browser_can(S_OPEN));
    enable(M_SERVER, S_UP, browser && browser_can(S_UP));
    enable(M_SERVER, S_DISCONNECT, browser && browser_can(S_DISCONNECT));
    for (i = V_PICTORIAL; i <= V_CHRONO; i++) {
        enable(M_HOUSE, i, browser);
        CheckItem(&menus[M_HOUSE - 1], i, i == view_mode());
    }
}

static void activate(void)
{
    int i;

    for (i = 0; i < N_MENUS; i++)
        InsertMenu(&menus[i], 0);
    menus_update();
    DrawMenuBar();
    SetPort(activeFolder);
    InitCursor();
}

static void command(Integer menu, Integer item)
{
    doc *d = active_doc();

    switch (menu) {
    case M_FILE:
        if (item == F_SETASIDEALL)
            DoFilingCmd(cmdClosAll);
        else if (item == F_SETASIDE)
            DoFilingCmd(cmdClose);
        break;
    case M_EDIT:
        if (item == E_COPY && d && d->kind == BROWSER) {
            browser_copy(d->w);
            owns_scrap = 1;
            redraw_browsers();
        } else if (item == E_COPYLW && d) {
            if (d->kind == BROWSER)
                browser_copy_lw(d->w);
            else
                browser_copy_macfile_lw(d->w, d->prefix);
            owns_scrap = 1;
            redraw_browsers();
        } else if (item == E_PASTE && d && d->kind == BROWSER) {
            browser_paste();
            redraw_browsers();
        } else if (item == E_PASTETEXT && d && d->kind == BROWSER) {
            browser_paste_text();
            redraw_browsers();
        }
        break;
    case M_SERVER:
        if (d && d->kind == BROWSER) {
            browser_command(item);
            redraw_browsers();
        }
        break;
    case M_HOUSE:
        if (d && d->kind == BROWSER && item >= V_PICTORIAL && item <= V_CHRONO)
            view_set_mode(d->w, item);
        break;
    }
    menus_update();
}

/* Open a document in the window the Filer gave us. */
static void open_doc(WindowPtr w, const char *prefix)
{
    doc *d = find_doc(w);
    int i;

    if (!d)
        for (i = 0; i < MAXDOCS && !d; i++)
            if (docs[i].kind == NONE)
                d = &docs[i];
    if (!d)
        return;
    d->w = w;
    snprintf(d->prefix, sizeof d->prefix, "%s", prefix);
    d->kind = (prefix[0] && macfile_is(prefix)) ? MACFILE : BROWSER;
    TakeWindow(w);
    draw(d);
    if (w == activeFolder)
        activate();
}

static void filer_event(EventRecord *ev, int *done)
{
    FilerExt fx;
    Integer err;
    char prefix[256], source[256];
    doc *d;
    int32_t e;

    GetAddParms(&err, ev, &fx);
    cstr(prefix, fx.thePrefix, sizeof prefix);
    cstr(source, fx.theResult, sizeof source);
    d = find_doc(ev->who);
    switch (fx.theFlrOp) {
    case fcResume:
    case fcNone:
        open_doc(ev->who, fx.theFlrOp == fcResume ? prefix : "");
        break;
    case fcCopy:
        if (strncmp(source, "-L", 2) == 0 && source[3] == '-') {
            /* a reference we put on the Clipboard (browser.c, browser_download) */
            e = browser_download(source, prefix);
            TellFiler(&err, e ? docNotXfered : docXfered, e ? cantRead : allOK, ev->who);
            redraw_browsers();
        } else {                                     /* the Filer copies one of our open documents */
            e = (d && d->kind == MACFILE) ? macfile_copy(d->prefix, prefix) : 0;
            TellFiler(&err, e ? docNotXfered : docXfered, e ? cantWrite : allOK, ev->who);
        }
        break;
    case fcPut:                                      /* to a new place, then close */
        e = (d && d->kind == MACFILE) ? macfile_copy(d->prefix, prefix) : 0;
        TellFiler(&err, e ? docNotXfered : docXfered, e ? cantWrite : allOK, ev->who);
        if (d && !e)
            d->kind = NONE;
        break;
    case fcClose:
    case fcSuspend:
    case fcShred:
        TellFiler(&err, docClosd, allOK, ev->who);
        if (d)
            d->kind = NONE;
        break;
    case fcTerminate:
        *done = 1;
        break;
    default:
        TellFiler(&err, docClosd, internalError, ev->who);
        break;
    }
}

int main(void)
{
    static const unsigned char excep_name[] = "\015SYS_TERMINATE";
    unsigned char name[18], prefix[64];
    EventRecord ev;
    Integer err;
    int done = 0;
    doc *d;

    InitGraf(&thePort);
    OpenWM();
    memcpy(name, excep_name, sizeof excep_name);
    lisa_declare_excep_hdl(&err, name, (void *)LastWishes);
    SetPort(deskPort);
    InitHeap(heap, heap + sizeof heap, (void *)HeapFull);
    tool_prefix(prefix);
    InitMP(theHeap, prefix, (void *)FailedAlert, 2, menus, &alertFile, &err);
    if (err > 0) {
        TellFiler(&err, InitFailed, internalError, 0);
        ImDying();
        return 1;
    }
    InitWmlCrs(&err);
    view_init();
    status("Choose Find Servers from the Server menu.");

    while (!done) {
        /* Block in GetEvent while inactive; while active, poll so that the
         * network's housekeeping (tickles) runs, and let others run. */
        if (ImActive() && !EventAvail()) {
            browser_idle();
            LetOthersRun();
            continue;
        }
        GetEvent(&ev);
        switch (ev.what) {
        case filerEvent:
            filer_event(&ev, &done);
            break;
        case folderActivate:
            TakeControl(&ev, false, false);
            if ((d = find_doc(ev.who)) != 0) {
                activate();
                if (d->kind == BROWSER)
                    view_activate(d->w, 1);
            }
            break;
        case folderDeactivate:
            if ((d = find_doc(ev.who)) != 0 && d->kind == BROWSER)
                view_activate(d->w, 0);
            GiveControl(&ev);
            break;
        case folderUpdate:
            BeginUpdate(ev.who);
            if ((d = find_doc(ev.who)) != 0)
                draw(d);
            else if (ev.who == scrapFolder && owns_scrap)
                DrawRefScrap(&err);
            EndUpdate(ev.who);
            break;
        case buttonDown:
            if (ev.who == menuFolder) {
                Integer menu, item;
                MenuSelect(ev.where, &menu, &item);
                command(menu, item);
                HiLiteMenu(0);
            } else if ((d = find_doc(ev.who)) != 0 && d->kind == BROWSER) {
                SetPort(d->w);
                view_click(d->w, ev.where, ev.when);
                redraw_browsers();
                menus_update();
            }
            break;
        }
    }
    browser_shutdown();
    ImDying();
    return 0;
}
