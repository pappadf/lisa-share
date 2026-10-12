/* serialb: can a desktop tool take over Serial B at run time?
 *
 * The AppleTalk driver is meant to ship on the tool's floppy and be attached
 * to Serial B only while the tool runs (CDKILL the serial driver, CDMAKE ours,
 * and the reverse on quit), with no installer and no Preferences step.  This
 * probe checks the OS side of that with the driver that is already there:
 *
 *   1. OSBOOTVOL, and CDINFO on the SCC (slot 10) and its channels A and B;
 *   2. CDKILL on Serial B, then CDINFO (expect 751, no device);
 *   3. CDMAKE on Serial B with the info CDINFO returned in step 1, then CDINFO
 *      (expect the same driver back), then MOUNT it: CDKILL unmounted the
 *      device, and a device attached with CDMAKE starts unmounted.
 *
 * One line per call (probes/common/probe.h).  Positions
 * are 1-based: slot 10 = the built-in serial ports, chan 1 = A, chan 2 = B. */
#include <string.h>
#include "lisa.h"
#include "lisa_os.h"
#include "lisa/ostypes.h"
#include "probe.h"

const char probe_tool_number[] = "23";
const char probe_results[] = "SERIALB.RESULTS";

static const char *devt_name(int t)
{
    static const char *names[] = {"diskdev", "pascalbd", "seqdev", "bitbkt", "non_io"};
    return t < 5 ? names[t] : "?";
}

static int info(const char *label, int slot, int chan, cd_infobuf *buf)
{
    Integer err;
    char name[33];

    memset(buf, 0, sizeof *buf);
    lisa_cdinfo(&err, CD_POS(slot, chan, 0), buf);
    if (err > 0) {
        say("CDINFO %s (%d,%d): error %d", label, slot, chan, err);
        return err;
    }
    cstr(name, buf->cd_drvrname);
    say("CDINFO %s (%d,%d): id %ld '%s' %s%s", label, slot, chan,
        (long)buf->cd_driverid, name, devt_name(buf->cd_devt), buf->cd_perm ? " perm" : "");
    return 0;
}

void run_probe(void)
{
    unsigned char vol[34];
    char name[33];
    cd_infobuf scc, a, b, after;
    Integer err;

    lisa_osbootvol(&err, vol);
    cstr(name, vol);
    say("OSBOOTVOL: error %d, '%s'", err, name);
    info("SCC", 10, 0, &scc);
    info("Serial A", 10, 1, &a);
    if (info("Serial B", 10, 2, &b) > 0)
        return;

    lisa_cdkill(&err, CD_POS(10, 2, 0));
    say("CDKILL Serial B: error %d", err);
    if (err > 0)
        return;
    info("Serial B", 10, 2, &after);

    lisa_cdmake(&err, CD_POS(10, 2, 0), &b);
    say("CDMAKE Serial B (as before): error %d", err);
    info("Serial B", 10, 2, &after);
    {
        unsigned char vname[34], password[34] = {0}, devname[34];
        Integer merr;

        pstr(devname, "RS232B");
        lisa_mount(&merr, vname, password, devname);
        say("MOUNT RS232B: error %d", merr);
        if (merr > 0)
            err = merr;
    }
    say(err <= 0 && after.cd_driverid == b.cd_driverid ? "RESULT: Serial B detached and restored"
                                                         : "RESULT: Serial B NOT restored");
}
