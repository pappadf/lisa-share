/* afp: read a file from an AppleShare server.
 *
 * With the LocalTalk driver installed (lib/atk_link.c), the probe finds the
 * file server with NBP, asks its status with ASP GetStatus, opens an ASP
 * session, logs in as a guest with AFP, lists the server's volumes, opens
 * the first one, lists its top directory and reads the data fork of the
 * first file in it.  make test publishes a share with a text file on
 * granny-smith's AppleTalk network before the probe runs. */
#include <stdio.h>
#include <string.h>
#include "probe.h"
#include "atk.h"

const char probe_tool_number[] = "27";
const char probe_results[] = "AFP.RESULTS";

static void type_creator(const uint8_t *finder, char *out)
{
    int i;

    for (i = 0; i < 8; i++)
        out[i + (i >= 4)] = (finder[i] >= 32 && finder[i] < 127) ? (char)finder[i] : '?';
    out[4] = '/';
    out[9] = 0;
}

static void session(const nbp_entity *server)
{
    static uint8_t status[8 * ATP_MAX_DATA];
    static uint8_t data[512];
    char vols[4][28], tc[10], text[60];
    afp_entry e[8];
    asp_session s;
    uint16_t vol, fork;
    int32_t err;
    int len, n, i, got;

    err = asp_get_status(server->node, server->socket, status, sizeof status, &len);
    if (err) {
        say("ASP GetStatus: error %ld", (long)err);
        return;
    }
    say("ASP GetStatus: %d bytes, server \"%.*s\"", len, len > 10 ? status[10] : 0, (const char *)status + 11);
    err = asp_open(&s, server->node, server->socket);
    say("ASP OpenSession: error %ld, session %d, server socket %d", (long)err, s.sid, s.sss);
    if (err)
        return;
    err = afp_login(&s);
    say("AFP FPLogin (guest, AFPVersion 2.0): %ld", (long)err);
    if (err)
        goto close;
    err = afp_volumes(&s, vols, 4, &n);
    say("AFP FPGetSrvrParms: %ld, %d volume(s)%s%s", (long)err, n, n ? ", the first: " : "", n ? vols[0] : "");
    if (err || n == 0)
        goto logout;
    err = afp_open_vol(&s, vols[0], &vol);
    say("AFP FPOpenVol \"%s\": %ld, volume ID %d", vols[0], (long)err, vol);
    if (err)
        goto logout;
    err = afp_enumerate(&s, vol, AFP_ROOT, "", 1, e, 8, &n);
    say("AFP FPEnumerate: %ld, %d item(s)", (long)err, n);
    for (i = 0; i < n; i++) {
        if (e[i].is_dir) {
            say("  folder \"%s\", ID %lu, %d items", e[i].name, (unsigned long)e[i].id, e[i].offspring);
        } else {
            type_creator(e[i].finder, tc);
            say("  file \"%s\", %lu + %lu bytes, %s", e[i].name, (unsigned long)e[i].data_len,
                (unsigned long)e[i].rsrc_len, tc);
        }
    }
    for (i = 0; i < n && (e[i].is_dir || e[i].data_len == 0); i++)
        ;
    if (i < n) {
        err = afp_open_fork(&s, vol, AFP_ROOT, e[i].name, 0, &fork);
        say("AFP FPOpenFork \"%s\": %ld, fork %d", e[i].name, (long)err, fork);
        if (!err) {
            err = afp_read(&s, fork, 0, sizeof data, data, &got);
            for (len = 0; len < got && len < 59 && data[len] >= 32; len++)
                text[len] = (char)data[len];
            text[len] = 0;
            say("AFP FPRead: %ld, %d bytes: \"%s\"", (long)err, got, text);
            if (got > 0 && (err == 0 || err == AFP_EOF))
                say("RESULT: file read: %s", text);
            afp_close_fork(&s, fork);
        }
    }
    afp_close_vol(&s, vol);
logout:
    afp_logout(&s);
close:
    asp_close(&s);
    say("Session closed");
}

void run_probe(void)
{
    nbp_entity server[2];
    int err, n;

    err = atk_open();
    if (err) {
        say("Installing the driver failed: %s, error %d", atk_failed_step(), err);
        return;
    }
    n = nbp_lookup("=", "AFPServer", "*", server, 2, 100, 3);
    if (n == 0) {
        say("No file server found");
    } else {
        say("File server \"%s\" at node %d socket %d", server[0].object, server[0].node, server[0].socket);
        session(&server[0]);
    }
    atk_close();
}
