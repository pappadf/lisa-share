/* The browser's window, drawn the way the Desktop Manager draws a disk or
 * folder, so that the place a file is copied from looks like the place it is
 * pasted to.  Housekeeping offers the Desktop Manager's three views:
 *   - Pictorial View: icons on a 120 x 43 grid, the name centered under each
 *     in the "tile7" font (APDM-DESK DrawTheIcon, GetNameLoc);
 *   - Alphabetical View, Chronological View: a list (APDM-DESK DrawList,
 *     DrawRow) with column titles Title, Size, Modified and Created in the
 *     system font, underlined; per row the small icon at x 10, the name in
 *     "p10Tile" from x 40, the size right-aligned at 320, the times and dates
 *     right-aligned in fields ending at 500 and 680.  Chronological puts the
 *     most recently modified first (SortContents).
 * The icons are the Desktop Manager's own: characters of the icon font
 * (fIconFont) and, in the list, of the window manager font.  Their codes are
 * FilerComm's (LIBFC/FCIMPL InitIcons); its tables iconData, tinyData etc.
 * are filled only in the Desktop Manager's process, so they are copied here.
 * A selected icon is inverted with its mask.
 * Below the view a status line says where the browser is and what happened.
 * Scroll bars and the size box are the Office System's (WM library), as in
 * LisaPaint. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lisashare.h"

enum {
    TINY_START = 10, NAME_START = 40, NAME_END = 270, SIZE_END = 320, MOD_END = 500, CREAT_END = 680,
    H_GRID = 120, V_GRID = 43, H_GRID_START = 36, V_GRID_START = 2, ICON_HALF_W = 24, ICON_HT = 32,
    BLACK_SYMBOL = 65, STATUS_H = 18, MIN_W = 300, MIN_H = 120,
};

/* icon, icon mask, small icon, small icon mask */
static const struct glyph { int kind; char icon, mask, tiny, tiny_mask; } glyphs[] = {
    {folderkind, 4, 5, 82, 83},
    {docKind, 6, 7, 80, 81},
    {computerKind, 18, 19, 104, 105},
    {diskKind, 38, 39, 118, 119},
};

static int mode = V_ALPHA;
static TSbList sbList;
static THSb hsbH, hsbV;
static Rect rH, rV, content, statusR;
static int top_row;                     /* the first row (list) or icon row (pictorial) shown */
static int order[64], norder;
static int line_ht, base_off, date_w, time_w, icon_ascent;
static int32_t last_click;
static int last_hit = -1;

static const struct glyph *glyph(int kind)
{
    unsigned i;

    for (i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++)
        if (glyphs[i].kind == kind)
            return &glyphs[i];
    return &glyphs[1];                  /* a document */
}

static Point mkpt(int h, int v)
{
    Point p;

    p.h = (Integer)h;
    p.v = (Integer)v;
    return p;
}

static void text_at(int h, int v, const char *s)
{
    unsigned char p[256];

    MoveTo((Integer)h, (Integer)v);
    pstr(p, s);
    DrawString(p);
}

static int text_width(const char *s)
{
    unsigned char p[256];

    pstr(p, s);
    return StringWidth(p);
}

void view_init(void)
{
    FontInfo fi;

    InitWmlSb();
    InitSbList(&sbList, theHeap);
    hsbV = SbCreate(&sbList, hsbNil, tyvhV, mkpt(0, 0), 100);
    hsbH = SbCreate(&sbList, hsbNil, tyvhH, mkpt(0, 0), 100);
    /* the Desktop Manager's metrics (APDM-DESK, its initialization) */
    SetPort(deskPort);
    TextFont(tile7);
    GetFontInfo(&fi);
    icon_ascent = fi.ascent;
    TextFont(sysText);
    date_w = text_width(" 99/99/99");
    time_w = text_width("99:99 am");
    GetFontInfo(&fi);
    line_ht = fi.ascent + fi.descent + fi.leading;
    if (line_ht < 15)
        line_ht = 15;
    line_ht = (line_ht + 3) / 4 * 4;    /* a multiple of 4 */
    base_off = fi.descent + (line_ht - (fi.ascent + fi.descent) + 1) / 2;
}

int view_mode(void)
{
    return mode;
}

/* ---- the order of the rows ------------------------------------------------ */

static int ci_compare(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : (unsigned char)*a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : (unsigned char)*b;
        if (ca != cb || !ca)
            return ca - cb;
    }
}

static int32_t when(const view_row *r)
{
    return r->mdate ? r->mdate : r->cdate;
}

static void sort_rows(void)             /* rows fetched as needed: no copy of them all */
{
    view_row a, b;
    int i, j, t, swap;

    norder = browser_nrows();
    if (norder > 64)
        norder = 64;
    for (i = 0; i < norder; i++)
        order[i] = i;
    for (i = 1; i < norder; i++)        /* a simple insertion sort, as small as the lists are */
        for (j = i; j > 0; j--) {
            browser_row(order[j - 1], &a);
            browser_row(order[j], &b);
            if (mode == V_CHRONO && (a.has_dates || b.has_dates))
                swap = when(&a) < when(&b);         /* most recent first */
            else
                swap = ci_compare(a.name, b.name) > 0;
            if (!swap)
                break;
            t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
}

/* ---- layout and scrolling -------------------------------------------------- */

static int columns(void)
{
    int n = (content.right - content.left) / H_GRID;
    return n > 0 ? n : 1;
}

static int lines(void)                  /* rows (list) or icon rows (pictorial) that fit */
{
    int h = statusR.top - content.top;
    int n = mode == V_PICTORIAL ? h / V_GRID : (h - line_ht) / line_ht;
    return n > 0 ? n : 1;
}

static int total_lines(void)
{
    return mode == V_PICTORIAL ? (norder + columns() - 1) / columns() : norder;
}

static int max_top(void)
{
    int m = total_lines() - lines();
    return m > 0 ? m : 0;
}

static void set_thumbs(void)
{
    int m = max_top();

    SetSbIcons(hsbV, m ? sbAllIcons : 0);
    SetSbIcons(hsbH, 0);                /* everything fits across */
    SetThumb(hsbV, m ? (int)((long)top_row * 1000 / m) : 0);
    SetThumb(hsbH, 0);
}

static void layout(WindowPtr w)
{
    Rect pr = w->portRect;

    SetPort(w);
    FlushRects(&rH, &rV);
    SetSbRect(hsbH, &rH);
    SetSbRect(hsbV, &rV);
    SetRect(&content, pr.left, pr.top, rV.left, rH.top);
    SetRect(&statusR, content.left, content.bottom - STATUS_H, content.right, content.bottom);
    if (top_row > max_top())
        top_row = max_top();
    set_thumbs();
}

void view_changed(void)                 /* new contents: back to the top */
{
    top_row = 0;
    last_hit = -1;
}

/* ---- drawing --------------------------------------------------------------- */

/* "mm/dd/yy" and "h:mm am", as the Desktop Manager shows them */
static void format_time(int32_t t, char *date, char *time)
{
    time_t tt = t;
    struct tm tm;

    gmtime_r(&tt, &tm);                 /* the server's local time, as it gave it */
    sprintf(date, " %02d/%02d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
    sprintf(time, "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
}

static void right_at(int right, int v, const char *s)
{
    text_at(right - text_width(s), v, s);
}

/* The small list icon (APDM-DESK DrawSymbol) at pt, inverted if selected */
static void draw_tiny(int kind, int h, int v, int selected)
{
    const struct glyph *g = glyph(kind);

    TextFont(wmfont);
    MoveTo((Integer)h, (Integer)v);
    TextMode(srcBic);
    DrawChar((char)BLACK_SYMBOL);       /* erase the area */
    MoveTo((Integer)h, (Integer)v);
    TextMode(srcOr);
    DrawChar(g->tiny);
    if (selected) {
        MoveTo((Integer)h, (Integer)v);
        TextMode(srcXor);
        DrawChar(g->tiny_mask);
    }
    TextMode(srcOr);
}

static void draw_list(void)
{
    char date[16], time[16], s[16];
    view_row r;
    int i, top, bottom, sel = browser_sel();

    TextFont(sysText);                  /* the column titles */
    TextFace(underline);
    top = content.top;
    text_at(NAME_START, top + line_ht - base_off, "Title");
    right_at(SIZE_END, top + line_ht - base_off, "Size");
    text_at(MOD_END - (date_w + time_w) / 2 - text_width("Modified") / 2, top + line_ht - base_off, "Modified");
    text_at(CREAT_END - (date_w + time_w) / 2 - text_width("Created") / 2, top + line_ht - base_off, "Created");
    TextFace(0);

    for (i = top_row; i < norder && i - top_row < lines(); i++) {
        browser_row(order[i], &r);
        bottom = top + line_ht * (i - top_row + 2);
        draw_tiny(r.kind, TINY_START, bottom - base_off - 13, order[i] == sel);
        TextFont(p10Tile);
        text_at(NAME_START, bottom - base_off, r.name);
        TextFont(sysText);
        if (r.size >= 0) {
            sprintf(s, "%ld", (long)r.size);
            right_at(SIZE_END, bottom - base_off, s);
        }
        if (r.has_dates && r.mdate) {
            format_time(r.mdate, date, time);
            right_at(MOD_END - date_w, bottom - base_off, time);
            right_at(MOD_END, bottom - base_off, date);
        }
        if (r.has_dates && r.cdate) {
            format_time(r.cdate, date, time);
            right_at(CREAT_END - date_w, bottom - base_off, time);
            right_at(CREAT_END, bottom - base_off, date);
        }
    }
}

/* A large icon (APDM-DESK DrawTheIcon) with its name under it */
static void draw_icon(const view_row *r, int h, int v, int selected)
{
    const struct glyph *g = glyph(r->kind);

    TextFont(fIconFont);
    MoveTo((Integer)h, (Integer)v);
    TextMode(srcBic);
    DrawChar(g->mask);        /* punch a hole */
    MoveTo((Integer)h, (Integer)v);
    TextMode(srcOr);
    DrawChar(g->icon);
    if (selected) {
        MoveTo((Integer)h, (Integer)v);
        TextMode(srcXor);
        DrawChar(g->mask);
    }
    TextMode(srcOr);
    TextFont(tile7);
    text_at(h + ICON_HALF_W - text_width(r->name) / 2, v + ICON_HT + icon_ascent + 1, r->name);
}

static void draw_pictorial(void)
{
    view_row r;
    int i, col, row, cols = columns(), sel = browser_sel();

    for (i = top_row * cols; i < norder && i / cols - top_row < lines(); i++) {
        browser_row(order[i], &r);
        col = i % cols;
        row = i / cols - top_row;
        draw_icon(&r, content.left + col * H_GRID + H_GRID_START, content.top + row * V_GRID + V_GRID_START,
                  order[i] == sel);
    }
}

void view_draw(WindowPtr w, int active)
{
    char where[120];

    layout(w);
    sort_rows();
    if (top_row > max_top())
        top_row = max_top();
    set_thumbs();
    ClipRect(&w->portRect);
    EraseRect(&content);
    ClipRect(&content);
    if (mode == V_PICTORIAL)
        draw_pictorial();
    else
        draw_list();
    /* the status line: where, then what happened */
    ClipRect(&content);
    EraseRect(&statusR);
    MoveTo(statusR.left, statusR.top);
    LineTo(statusR.right, statusR.top);
    TextFont(sysText);
    browser_where(where, sizeof where);
    text_at(statusR.left + 8, statusR.bottom - 5, status_line[0] ? status_line : where);
    ClipRect(&w->portRect);
    if (active) {
        PaintSbar(hsbH);
        PaintSbar(hsbV);
        PaintGrow();
    }
}

void view_activate(WindowPtr w, int on)
{
    SetPort(w);
    ClipRect(&w->portRect);
    if (on) {
        layout(w);
        PaintSbar(hsbH);
        PaintSbar(hsbV);
        PaintGrow();
    } else {
        HideSBorder(&w->portRect);
    }
}

void view_set_mode(WindowPtr w, int m)
{
    mode = m;
    top_row = 0;
    if (w)
        view_draw(w, 1);
}

/* ---- clicks ---------------------------------------------------------------- */

static void scroll_to(WindowPtr w, int t)
{
    if (t < 0)
        t = 0;
    if (t > max_top())
        t = max_top();
    if (t == top_row)
        return;
    top_row = t;
    view_draw(w, 1);
}

static void scroll_bar_hit(WindowPtr w, THSb hsb, TIcon icon, Point where)
{
    int dir;
    Integer c = 0;

    if (hsb != hsbV)
        return;
    switch (icon) {
    case iconArwA:
    case iconArwB:                      /* a row at a time, while the button is down */
        dir = icon == iconArwA ? -1 : 1;
        PaintArw(hsb, icon, true);
        do {
            scroll_to(w, top_row + dir);
            PaintArw(hsb, icon, true);
        } while (StillDown());
        PaintArw(hsb, icon, false);
        break;
    case iconPagA: case iconGryA:
    case iconPagB: case iconGryB:       /* a window at a time */
        dir = (icon == iconPagA || icon == iconGryA) ? -1 : 1;
        scroll_to(w, top_row + dir * (lines() - 1 > 0 ? lines() - 1 : 1));
        break;
    case iconThumb:
        DragThumb(hsb, where, &c);
        scroll_to(w, (int)((long)c * max_top() / 1000));
        set_thumbs();
        PaintSbar(hsb);
        break;
    default:
        break;
    }
}

static void grow(WindowPtr w, Point where)
{
    Point br;

    DragFrame(where, true, &br);
    br.h = (Integer)(br.h - w->portRect.left < MIN_W ? MIN_W : br.h - w->portRect.left);
    br.v = (Integer)(br.v - w->portRect.top < MIN_H ? MIN_H : br.v - w->portRect.top);
    SetPort(w);
    HideSBorder(&w->portRect);
    FolderSize(w, br.h, br.v, true);
    layout(w);
    InvalRect(&w->portRect);
}

/* The row under a point, or -1 */
static int hit(Point where)
{
    int i;

    if (!PtInRect(where, &content) || where.v >= statusR.top)
        return -1;
    if (mode == V_PICTORIAL) {
        int col = (where.h - content.left) / H_GRID, row = (where.v - content.top) / V_GRID;
        if (col >= columns())
            return -1;
        i = (top_row + row) * columns() + col;
    } else {
        if (where.v < content.top + line_ht)
            return -1;                  /* the column titles */
        i = top_row + (where.v - content.top - line_ht) / line_ht;
    }
    return i < norder ? order[i] : -1;
}

void view_click(WindowPtr w, Point where, int32_t t)
{
    THSb hsb;
    TIcon icon;
    int i;

    SetPort(w);
    if (FGrowHit(where)) {
        grow(w, where);
        return;
    }
    if (FSbHit(&sbList, where, &hsb, &icon)) {
        scroll_bar_hit(w, hsb, icon, where);
        return;
    }
    i = hit(where);
    if (i >= 0 && i == last_hit && t - last_click < 50) {
        browser_set_sel(i);
        browser_open_sel();             /* a double click */
        last_hit = -1;
        return;
    }
    browser_set_sel(i);
    last_hit = i;
    last_click = t;
}
