/* The link: install SYSTEM.CD_ATALK at Serial B, open it, read and write
 * LLAP frames, and put the serial driver back (lib/atk.h).
 *
 * Installing a driver from a tool, with nothing for the user to set up
 * (lisa-c's README, "Configurable drivers"): copy the driver file from the
 * tool's floppy to the boot volume, where the OS loads drivers from; save
 * Serial B's driver (CDINFO) and remove it (CDKILL, which also unmounts the
 * device); attach ours (CDMAKE) and MOUNT it; open the device with a dummy
 * file name.  atk_close undoes each step.  Serial B may have no device at
 * all (the Preferences tool sets up a port only when something is connected
 * to it): then there is nothing to save, and atk_close leaves it empty. */
#include <stdio.h>
#include <string.h>
#include "lisa.h"
#include "lisa_os.h"
#include "lisa/ostypes.h"
#include "atk.h"

enum { M_DREAD = 1, M_DWRITE = 2 };     /* mset bits */
enum { IO_SEQUENTIAL = 2 };             /* iomode */
enum { DEVT_SEQDEV = 2 };               /* devtype */
enum { CD_NODEV = 751 };                /* no device at the position */

void ddp_deliver(const uint8_t *frame, int len);    /* atk_ddp.c */

static struct {
    int open;                           /* 0 none, 1 driver file copied, 2 attached, 3 device open */
    const char *step;
    unsigned char driver_path[80];
    int had_serial;                     /* Serial B had a driver ... */
    cd_infobuf serial;                  /* ... this one, to put back */
    Integer refnum;
    uint8_t node;
    uint8_t frame[LLAP_MAX_FRAME];
} link;

static void pstr(unsigned char *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static int fail(const char *step, int err)
{
    link.step = step;
    return err;
}

/* "-<boot volume>-" and "-<the tool's volume>-" */
static void volumes(char *boot, char *tool)
{
    static unsigned char info[600];
    unsigned char vol[34];
    Integer err;
    int n;

    lisa_osbootvol(&err, vol);
    snprintf(boot, 40, "-%.*s-", vol[0], (const char *)vol + 1);
    lisa_info_process(&err, lisa_my_id(), info);     /* progpathname: -<vol>-{Tn}obj */
    for (n = 2; n <= info[0] && info[n] != '-'; n++)
        ;
    snprintf(tool, 40, "-%.*s-", n - 2, (const char *)info + 2);
}

static int copy_file(const unsigned char *src, const unsigned char *dst)
{
    static char buf[512];
    Integer err, e2, in, out;
    int32_t got, put;

    lisa_kill_object(&err, dst);        /* an older copy */
    lisa_make_file(&err, dst, 0);
    if (err > 0)
        return err;
    lisa_open(&err, src, &in, M_DREAD);
    if (err > 0)
        return err;
    lisa_open(&err, dst, &out, M_DWRITE);
    if (err > 0) {
        lisa_close_object(&e2, in);
        return err;
    }
    for (;;) {
        lisa_read_data(&err, in, buf, sizeof buf, &got, IO_SEQUENTIAL, 0);
        if (got <= 0)
            break;
        lisa_write_data(&e2, out, buf, got, &put, IO_SEQUENTIAL, 0);
        if (e2 > 0) {
            err = e2;
            break;
        }
    }
    if (err == 848 || err == 956)       /* end of file */
        err = 0;
    lisa_close_object(&e2, in);
    lisa_close_object(&e2, out);
    return err > 0 ? err : 0;
}

static void mount_serial_b(void)
{
    unsigned char vname[34], password[34] = {0}, devname[34];
    Integer err;

    pstr(devname, "RS232B");
    lisa_mount(&err, vname, password, devname);
}

int atk_control(int code, int32_t data[10])
{
    unsigned char dev[16];
    dctype dc;
    Integer err;

    memset(&dc, 0, sizeof dc);
    dc.dcversion = 2;
    dc.dccode = (Integer)code;
    memcpy(dc.dcdata, data, sizeof dc.dcdata);
    pstr(dev, "-RS232B");
    lisa_device_control(&err, dev, &dc);
    memcpy(data, dc.dcdata, sizeof dc.dcdata);
    return err;
}

int atk_open(void)
{
    char boot[40], tool[40], s[80];
    unsigned char src[80], file[20];
    cd_infobuf atk;
    int32_t data[10];
    Integer err;

    if (link.open)
        return 0;
    volumes(boot, tool);
    snprintf(s, sizeof s, "%sSYSTEM.CD_%s", tool, ATK_DRIVER_NAME);
    pstr(src, s);
    snprintf(s, sizeof s, "%sSYSTEM.CD_%s", boot, ATK_DRIVER_NAME);
    pstr(link.driver_path, s);
    if ((err = copy_file(src, link.driver_path)) > 0)
        return fail("copying the driver to the boot volume", err);
    link.open = 1;

    memset(&link.serial, 0, sizeof link.serial);
    lisa_cdinfo(&err, CD_POS(10, 2, 0), &link.serial);
    link.had_serial = err != CD_NODEV;
    if (err > 0 && link.had_serial)
        return atk_close(), fail("CDINFO Serial B", err);
    if (link.had_serial) {
        lisa_cdkill(&err, CD_POS(10, 2, 0));
        if (err > 0)
            return atk_close(), fail("CDKILL Serial B (is it in use?)", err);
    }
    memset(&atk, 0, sizeof atk);
    atk.cd_driverid = ATK_DRIVER_ID;
    pstr(atk.cd_drvrname, ATK_DRIVER_NAME);
    atk.cd_devt = DEVT_SEQDEV;
    lisa_cdmake(&err, CD_POS(10, 2, 0), &atk);
    link.open = 2;
    if (err > 0)
        return atk_close(), fail("CDMAKE the LocalTalk driver", err);
    mount_serial_b();

    pstr(file, "-RS232B-ATALK");
    lisa_open(&err, file, &link.refnum, M_DREAD | M_DWRITE);
    if (err > 0)
        return atk_close(), fail("opening the LocalTalk port", err);
    link.open = 3;
    memset(data, 0, sizeof data);
    data[0] = 1;
    atk_control(ATK_SET_NONBLOCK, data);
    memset(data, 0, sizeof data);
    atk_control(ATK_STATUS, data);
    link.node = (uint8_t)data[0];
    link.step = 0;
    return 0;
}

void atk_close(void)
{
    Integer err;

    if (link.open >= 3)
        lisa_close_object(&err, link.refnum);
    if (link.open >= 2) {
        lisa_cdkill(&err, CD_POS(10, 2, 0));
        if (link.had_serial) {
            lisa_cdmake(&err, CD_POS(10, 2, 0), &link.serial);
            mount_serial_b();
        }
    }
    if (link.open >= 1)
        lisa_kill_object(&err, link.driver_path);
    link.open = 0;
    link.node = 0;
}

const char *atk_failed_step(void)
{
    return link.step ? link.step : "";
}

uint8_t atk_node(void)
{
    return link.node;
}

uint32_t atk_time_us(void)
{
    int32_t data[10] = {0};

    atk_control(ATK_TIME, data);
    return (uint32_t)data[0];
}

int atk_status(int32_t data[10])
{
    memset(data, 0, 10 * sizeof data[0]);
    return atk_control(ATK_STATUS, data);
}

int atk_send_frame(const uint8_t *frame, int len)
{
    Integer err;
    int32_t actual;

    if (link.open < 3)
        return ATK_E_FUNCTION;
    lisa_write_data(&err, link.refnum, frame, len, &actual, IO_SEQUENTIAL, 0);
    return err > 0 ? err : 0;
}

int atk_poll(void)
{
    Integer err;
    int32_t actual;
    int count = 0;

    while (link.open >= 3) {
        lisa_read_data(&err, link.refnum, link.frame, sizeof link.frame, &actual, IO_SEQUENTIAL, 0);
        if (err > 0 || actual <= 0)
            break;
        if (link.frame[2] == LLAP_DDP_SHORT)
            ddp_deliver(link.frame, (int)actual);
        count++;
    }
    return count;
}

void atk_wait_ms(int ms)
{
    uint32_t start = atk_time_us();

    while (atk_time_us() - start < (uint32_t)ms * 1000u)
        atk_poll();
}
