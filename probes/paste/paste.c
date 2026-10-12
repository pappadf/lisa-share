/* paste: can a tool create a document on the desktop through the Clipboard?
 *
 * This is how lisa-share puts a file from a server on the Lisa desktop
 * (docs/architecture.md): the tool puts an icon reference on the Clipboard
 * (LibFC StartIconRef / AddIconRef / EndIconRef), the user pastes it into a
 * disk or folder, and the Desktop Manager (APDM-DESK2 PasteRef) makes a
 * catalog entry for a new document of the tool and, because the reference
 * is not its own, sends the tool fcCopy with the new document's file prefix
 * (thePrefix) and the reference's private name (theResult).  The tool writes
 * the document's files under the prefix and answers docXfered.
 *
 * The probe puts a reference to "Pasted Document" on the Clipboard when its
 * own document opens; make test then selects the floppy on the desktop and
 * chooses Edit > Paste.  The probe writes PASTED.TXT under the prefix. */
#include <stdio.h>
#include <string.h>
#include "lisa.h"
#include "lisa_os.h"
#include "lisa/desktop.h"
#include "probe.h"

const char probe_tool_number[] = "28";
const char probe_results[] = "PASTE.RESULTS";

enum { TOOL = 28, M_DWRITE = 2, IO_SEQUENTIAL = 2 };

void run_probe(void)
{
    TIconRef ref;
    Integer err;

    memset(&ref, 0, sizeof ref);
    ref.kind = docKind;
    ref.toolID = TOOL;
    pstr(ref.userName, "Pasted Document");
    pstr(ref.diskName, "-PASTE-SOURCE1");
    StartIconRef(&err, (WindowPtr)probe_window());
    if (err > 0) {
        say("StartIconRef: error %d", err);
        return;
    }
    AddIconRef(&ref);
    EndIconRef();
    say("Icon reference to \"Pasted Document\" on the Clipboard");
    probe_save();
    DoFilingCmd(cmdClose);              /* set our document aside: the disk's window is in front */
}

void probe_draw_scrap(void)
{
    Integer err;

    DrawRefScrap(&err);
}

/* The Desktop Manager pasted our reference: write the new document. */
int probe_filer(int op, void *who, const void *fxp)
{
    const FilerExt *fx = fxp;
    static const char text[] = "Written by the paste probe for fcCopy.\n";
    char prefix[256], source[256], s[300];
    unsigned char path[256];
    Integer err, e2, refnum;
    int32_t actual;

    if (op != fcCopy)
        return 0;
    snprintf(prefix, sizeof prefix, "%.*s", fx->thePrefix[0], (const char *)fx->thePrefix + 1);
    snprintf(source, sizeof source, "%.*s", fx->theResult[0], (const char *)fx->theResult + 1);
    say("fcCopy: new document \"%s\", source \"%s\"", prefix, source);
    snprintf(s, sizeof s, "%sPASTED.TXT", prefix);
    pstr(path, s);
    lisa_make_file(&err, path, 0);
    if (err <= 0)
        lisa_open(&err, path, &refnum, M_DWRITE);
    if (err <= 0) {
        lisa_write_data(&err, refnum, text, sizeof text - 1, &actual, IO_SEQUENTIAL, 0);
        lisa_close_object(&e2, refnum);
    }
    say("Wrote %s: error %d", s, err);
    TellFiler(&e2, docXfered, err > 0 ? cantWrite : allOK, (WindowPtr)who);
    say(err <= 0 ? "RESULT: document created by paste" : "RESULT: could not write the document");
    probe_save();
    return 1;
}
