/* The common part of lisa-share's probes: each probe is a desktop tool whose
 * document window shows the steps of one test, one line each.
 *
 * A probe defines probe_tool_number (its {Tn} number, as a string),
 * probe_results (the name of the file the lines are also written to, on the
 * probe's own floppy, for make test) and run_probe(), which the skeleton
 * (probe.c) calls once, when the first document opens. */
#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>
#include <string.h>

extern const char probe_tool_number[];
extern const char probe_results[];
void run_probe(void);

void say(const char *fmt, ...);         /* add a line */
void probe_save(void);                  /* write the lines to the results file now */
void *probe_window(void);               /* the document window (a WindowPtr) */

/* Optional (weak; define them to use them): Filer events other than opening
   a document (fx is a FilerExt *, who the event's window), returning 1 if
   handled -- the probe then answers the Filer itself; and an update of the
   Clipboard window while the probe owns the Clipboard. */
int probe_filer(int op, void *who, const void *fx);
void probe_draw_scrap(void);

static inline void pstr(unsigned char *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static inline void cstr(char *out, const unsigned char *p)
{
    int n = p[0] > 32 ? 32 : p[0];
    memcpy(out, p + 1, (size_t)n);
    out[n] = 0;
}
#endif
