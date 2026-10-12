/* nbp: look up names on the network.
 *
 * Installs the LocalTalk driver (lib/atk_link.c) and makes two NBP lookups
 * (lib/atk_nbp.c): every entity (=:=@*), then the AppleShare file servers
 * (=:AFPServer@*).  granny-smith's AppleTalk node registers its file server,
 * its LaserWriter and its program-linking peer. */
#include "probe.h"
#include "atk.h"

const char probe_tool_number[] = "26";
const char probe_results[] = "NBP.RESULTS";

static void list(const char *what, const nbp_entity *e, int n)
{
    int i;

    say("Lookup %s: %d found", what, n);
    for (i = 0; i < n; i++)
        say("  %s:%s@%s  node %d socket %d", e[i].object, e[i].type, e[i].zone, e[i].node, e[i].socket);
}

void run_probe(void)
{
    nbp_entity e[8];
    int err, n;

    err = atk_open();
    if (err) {
        say("Installing the driver failed: %s, error %d", atk_failed_step(), err);
        return;
    }
    say("LocalTalk node %d", atk_node());

    n = nbp_lookup("=", "=", "*", e, 8, 100, 3);
    list("=:=@*", e, n);
    n = nbp_lookup("=", "AFPServer", "*", e, 8, 100, 3);
    list("=:AFPServer@*", e, n);
    say(n > 0 ? "RESULT: file server found" : "RESULT: no file server");

    atk_close();
    say("Serial driver restored");
}
