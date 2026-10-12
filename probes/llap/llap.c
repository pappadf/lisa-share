/* llap: install the LocalTalk driver and talk to the network.
 *
 * The floppy carries SYSTEM.CD_ATALK (driver/).  The probe
 *
 *   1. installs it at Serial B and opens it (lib/atk_link.c: copy to the boot
 *      volume, CDKILL the serial driver, CDMAKE ours, MOUNT, OPEN), which
 *      initializes the SCC for LocalTalk and acquires a node ID with lapENQ;
 *   2. acquires again starting at node 33, which the emulated network's own
 *      node owns: its lapACK must make the driver pick another;
 *   3. sends an AppleTalk Echo Protocol request to node 33, socket 4 (a
 *      directed frame: lapRTS, lapCTS, data) and waits for the echo reply on
 *      its own DDP socket (the network's lapRTS is answered with lapCTS by
 *      the driver's interrupt code);
 *   4. shows the driver's counters and puts the serial driver back. */
#include <string.h>
#include "probe.h"
#include "atk.h"

const char probe_tool_number[] = "25";
const char probe_results[] = "LLAP.RESULTS";

enum { PEER = 33, ECHO_SOCKET = 4, MY_SOCKET = 0xF0, AEP_REQUEST = 1, AEP_REPLY = 2 };

static const char ping[] = "Lisa ping";
static int replies;

static void echo_reply(void *ctx, const ddp_dgram *d)
{
    char text[40];
    int n = d->len - 1;

    (void)ctx;
    if (n > 39)
        n = 39;
    memcpy(text, d->data + 1, n > 0 ? (size_t)n : 0);
    text[n > 0 ? n : 0] = 0;
    say("Reply from node %d socket %d, DDP type %d, AEP function %d: \"%s\"", d->src_node, d->src_socket,
        d->type, d->len > 0 ? d->data[0] : -1, text);
    if (d->src_node == PEER && d->type == DDP_AEP && d->len > 0 && d->data[0] == AEP_REPLY &&
        strcmp(text, ping) == 0)
        replies++;
}

void run_probe(void)
{
    uint8_t req[1 + sizeof ping];
    int32_t data[10];
    int err, i;

    err = atk_open();
    if (err) {
        say("Installing the driver failed: %s, error %d", atk_failed_step(), err);
        return;
    }
    say("Driver installed at Serial B; node ID %d", atk_node());

    memset(data, 0, sizeof data);
    data[0] = PEER;
    err = atk_control(ATK_ACQUIRE, data);
    say("Acquire starting at %d: node %ld (error %d)%s", PEER, (long)data[0], err,
        data[0] != PEER ? ", 33 is taken" : ", 33 NOT detected");

    ddp_open(MY_SOCKET, echo_reply, 0);
    req[0] = AEP_REQUEST;
    memcpy(req + 1, ping, sizeof ping - 1);
    err = ddp_send(PEER, ECHO_SOCKET, MY_SOCKET, DDP_AEP, req, (int)sizeof ping);
    say("AEP Echo Request to node %d: error %d", PEER, err);
    for (i = 0; i < 50 && !replies; i++)
        atk_wait_ms(10);
    ddp_close(MY_SOCKET);
    say(replies ? "RESULT: echo reply received" : "RESULT: no echo reply");

    atk_status(data);
    say("Counters: received %ld, dropped %ld, bad %ld, sent %ld, failed %ld", (long)data[2],
        (long)data[3], (long)data[4], (long)data[5], (long)data[6]);
    say("  lapCTS sent %ld, lapACK sent %ld, lapENQ sent %ld", (long)data[7], (long)data[8], (long)data[9]);
    atk_close();
    say("Serial driver restored");
}
