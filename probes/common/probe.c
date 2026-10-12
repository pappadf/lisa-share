/* The probe skeleton (probe.h): an Office System desktop tool, in the startup
 * order of lisa-c's examples/hellotool, whose document shows the lines the
 * probe says.  After the probe has run, the lines are also written to
 * -<the tool's volume>-<probe_results>, so that make test can read them back
 * from the saved floppy instead of comparing screenshots. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lisa.h"
#include "lisa_os.h"
#include "lisa/desktop.h"
#include "probe.h"

enum { mSetAsideAll = 1, mSetAside = 2 };       /* File/Print items */
enum { M_DWRITE = 2, IO_SEQUENTIAL = 2 };

#define MAXLINES 20
static char lines[MAXLINES][90];
static int nlines;

static char heap[8192];
static MenuInfo menus[1];
static TAlertFile alertFile;
static WindowPtr myWindow;
static int ran;

void say(const char *fmt, ...)
{
    va_list ap;

    if (nlines == MAXLINES)
        return;
    va_start(ap, fmt);
    vsnprintf(lines[nlines++], sizeof lines[0], fmt, ap);
    va_end(ap);
}

static pascal Integer HeapFull(void *zone, Integer bytesNeeded)
{
    (void)zone; (void)bytesNeeded;
    return 0;
}

static pascal void FailedAlert(void)
{
}

static pascal void LastWishes(void *environment, void *data)
{
    (void)environment; (void)data;
    ImDying();
}

/* The program's path, -<volume>-{Tn}obj: the volume, and the prefix of the
   tool's files -<volume>-{Tn} */
static void program_path(char *vol, int size, unsigned char *prefix)
{
    static unsigned char info[600];
    Integer err;
    int n;

    lisa_info_process(&err, lisa_my_id(), info);
    for (n = 2; n <= info[0] && info[n] != '-'; n++)
        ;
    snprintf(vol, (size_t)size, "%.*s", n - 2, (const char *)info + 2);
    if (prefix) {
        char s[64];
        snprintf(s, sizeof s, "-%s-{T%s}", vol, probe_tool_number);
        pstr(prefix, s);
    }
}

__attribute__((weak)) int probe_filer(int op, void *who, const void *fx)
{
    (void)op; (void)who; (void)fx;
    return 0;
}

void *probe_window(void)
{
    return myWindow;
}

__attribute__((weak)) void probe_draw_scrap(void)
{
}

void probe_save(void)
{
    unsigned char path[80];
    char vol[40], s[80];
    Integer err, refnum;
    int32_t actual;
    int i;

    program_path(vol, sizeof vol, 0);
    snprintf(s, sizeof s, "-%s-%s", vol, probe_results);
    pstr(path, s);
    lisa_kill_object(&err, path);
    lisa_make_file(&err, path, 0);
    lisa_open(&err, path, &refnum, M_DWRITE);
    if (err > 0)
        return;
    for (i = 0; i < nlines; i++) {
        lisa_write_data(&err, refnum, lines[i], (int32_t)strlen(lines[i]), &actual, IO_SEQUENTIAL, 0);
        lisa_write_data(&err, refnum, "\n", 1, &actual, IO_SEQUENTIAL, 0);
    }
    lisa_close_object(&err, refnum);
}

static void draw(void)
{
    unsigned char s[92];
    Rect r = myWindow->portRect;
    int i;

    SetPort(myWindow);
    EraseRect(&r);
    for (i = 0; i < nlines; i++) {
        MoveTo(r.left + 10, r.top + 16 + 14 * i);
        pstr(s, lines[i]);
        DrawString(s);
    }
}

static void activate(void)
{
    InsertMenu(&menus[0], 0);
    DrawMenuBar();
    SetPort(myWindow);
    InitCursor();
}

static void menu_command(Integer menu, Integer item)
{
    if (menu != menus[0].menuId)
        return;
    switch (item) {
    case mSetAsideAll: DoFilingCmd(cmdClosAll); break;
    case mSetAside:    DoFilingCmd(cmdClose); break;
    }
}

static void filer_event(EventRecord *ev, int *done)
{
    FilerExt fx;
    Integer err;

    GetAddParms(&err, ev, &fx);
    switch (fx.theFlrOp) {
    case fcResume:
    case fcNone:
        myWindow = ev->who;
        TakeWindow(myWindow);
        if (!ran) {
            ran = 1;
            run_probe();
            probe_save();
        }
        draw();
        if (myWindow == activeFolder)
            activate();
        break;
    case fcCopy:
    case fcPut:
        if (probe_filer(fx.theFlrOp, ev->who, &fx))
            break;
        TellFiler(&err, docClosd, allOK, ev->who);
        myWindow = 0;
        break;
    case fcClose:
    case fcSuspend:
    case fcShred:
        TellFiler(&err, docClosd, allOK, ev->who);
        myWindow = 0;
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
    unsigned char name[18], prefix[42];
    char vol[40];
    EventRecord ev;
    Integer err;
    int done = 0;

    InitGraf(&thePort);
    OpenWM();
    memcpy(name, excep_name, sizeof excep_name);
    lisa_declare_excep_hdl(&err, name, (void *)LastWishes);
    SetPort(deskPort);
    InitHeap(heap, heap + sizeof heap, (void *)HeapFull);
    program_path(vol, sizeof vol, prefix);
    InitMP(theHeap, prefix, (void *)FailedAlert, 2, menus, &alertFile, &err);
    if (err > 0) {
        TellFiler(&err, InitFailed, internalError, 0);
        ImDying();
        return 1;
    }
    InitWmlCrs(&err);

    while (!done) {
        /* As the Office System tools do: block in GetEvent while inactive;
         * while active, poll and let the other processes run. */
        if (ImActive() && !EventAvail()) {
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
            activate();
            break;
        case folderDeactivate:
            GiveControl(&ev);
            break;
        case folderUpdate:
            BeginUpdate(ev.who);
            if (myWindow && ev.who == myWindow)
                draw();
            else if (ev.who == scrapFolder)
                probe_draw_scrap();
            EndUpdate(ev.who);
            break;
        case buttonDown:
            if (ev.who == menuFolder) {
                Integer menu, item;
                MenuSelect(ev.where, &menu, &item);
                menu_command(menu, item);
                HiLiteMenu(0);
            }
            break;
        }
    }
    ImDying();
    return 0;
}
