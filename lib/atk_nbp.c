/* NBP lookups (Inside AppleTalk, 2nd ed., ch. 7).
 *
 * An NBP packet is a control byte (function in the high nibble, tuple count
 * in the low), an NBP ID, then tuples.  A tuple is an entity address --
 * network (2 bytes), node, socket, enumerator -- and an entity name: object,
 * type and zone as Pascal strings.  On a network without a router a lookup is
 * a LkUp broadcast to the Names Information Socket (2) with one tuple, whose
 * address is where the replies go; each node with matching names answers
 * with LkUp-Reply packets of up to 15 tuples. */
#include <string.h>
#include "atk.h"

enum { NBP_SOCKET = 2, NBP_LKUP = 2, NBP_LKUP_REPLY = 3 };

static struct {
    uint8_t id;
    nbp_entity *out;
    int max, count;
} lookup;

/* A Pascal string at p (at most `room` bytes left) into s; returns its size
   with the length byte, or -1. */
static int get_pstring(const uint8_t *p, int room, char *s)
{
    int n;

    if (room < 1 || (n = p[0]) + 1 > room || n > 32)
        return -1;
    memcpy(s, p + 1, (size_t)n);
    s[n] = 0;
    return n + 1;
}

static int put_pstring(uint8_t *p, const char *s)
{
    int n = (int)strlen(s);

    if (n > 32)
        n = 32;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, (size_t)n);
    return n + 1;
}

static void reply(void *ctx, const ddp_dgram *d)
{
    const uint8_t *p = d->data;
    int left = d->len, tuples, i, j, n;
    nbp_entity e;

    (void)ctx;
    if (d->type != DDP_NBP || left < 2 || (p[0] >> 4) != NBP_LKUP_REPLY || p[1] != lookup.id)
        return;
    tuples = p[0] & 0x0F;
    p += 2;
    left -= 2;
    for (i = 0; i < tuples && lookup.count < lookup.max; i++) {
        if (left < 5)
            return;
        memset(&e, 0, sizeof e);
        e.net = (uint16_t)(p[0] << 8 | p[1]);
        e.node = p[2];
        e.socket = p[3];
        e.enumerator = p[4];
        p += 5;
        left -= 5;
        if ((n = get_pstring(p, left, e.object)) < 0)
            return;
        p += n, left -= n;
        if ((n = get_pstring(p, left, e.type)) < 0)
            return;
        p += n, left -= n;
        if ((n = get_pstring(p, left, e.zone)) < 0)
            return;
        p += n, left -= n;
        for (j = 0; j < lookup.count; j++)      /* answers repeat across retries */
            if (lookup.out[j].node == e.node && lookup.out[j].socket == e.socket &&
                lookup.out[j].enumerator == e.enumerator)
                break;
        if (j == lookup.count)
            lookup.out[lookup.count++] = e;
    }
}

int nbp_lookup(const char *object, const char *type, const char *zone, nbp_entity *out, int max,
               int interval_ms, int tries)
{
    uint8_t pkt[2 + 5 + 3 * 33];
    int socket, n, t;

    socket = ddp_open(0, reply, 0);
    if (socket < 0)
        return 0;
    lookup.id++;
    lookup.out = out;
    lookup.max = max;
    lookup.count = 0;

    n = 0;
    pkt[n++] = (uint8_t)(NBP_LKUP << 4 | 1);
    pkt[n++] = lookup.id;
    pkt[n++] = 0;                       /* the reply address: network 0, */
    pkt[n++] = 0;
    pkt[n++] = atk_node();              /* our node, */
    pkt[n++] = (uint8_t)socket;         /* this socket, */
    pkt[n++] = 0;                       /* enumerator 0 */
    n += put_pstring(pkt + n, object);
    n += put_pstring(pkt + n, type);
    n += put_pstring(pkt + n, zone);
    for (t = 0; t < tries; t++) {
        ddp_send(LLAP_BROADCAST, NBP_SOCKET, (uint8_t)socket, DDP_NBP, pkt, n);
        atk_wait_ms(interval_ms);
    }
    ddp_close((uint8_t)socket);
    return lookup.count;
}
