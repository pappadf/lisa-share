/* DDP on a LocalTalk network without a router: short headers (Inside
 * AppleTalk, 2nd ed., ch. 4, "DDP header formats").  The short header is 5
 * bytes after the LLAP header: 6 zero bits and the 10-bit datagram length
 * (header included), the destination socket, the source socket and the DDP
 * type. */
#include <string.h>
#include "atk.h"

static struct {
    ddp_handler handler;
    void *ctx;
} sockets[256];

int ddp_open(uint8_t socket, ddp_handler handler, void *ctx)
{
    int s = socket;

    if (!handler)
        return -1;
    if (s == 0) {                       /* a dynamic socket */
        for (s = 128; s < 255 && sockets[s].handler; s++)
            ;
        if (s == 255)
            return -1;
    } else if (sockets[s].handler) {
        return -1;
    }
    sockets[s].handler = handler;
    sockets[s].ctx = ctx;
    return s;
}

void ddp_close(uint8_t socket)
{
    sockets[socket].handler = 0;
    sockets[socket].ctx = 0;
}

int ddp_send(uint8_t dst_node, uint8_t dst_socket, uint8_t src_socket, uint8_t type,
             const void *data, int len)
{
    static uint8_t f[LLAP_MAX_FRAME];
    int total = 5 + len;

    if (len < 0 || len > DDP_MAX_DATA)
        return ATK_E_FRAME;
    f[0] = dst_node;
    f[1] = 0;                           /* the driver fills in our node */
    f[2] = LLAP_DDP_SHORT;
    f[3] = (uint8_t)((total >> 8) & 0x03);
    f[4] = (uint8_t)total;
    f[5] = dst_socket;
    f[6] = src_socket;
    f[7] = type;
    memcpy(f + 8, data, (size_t)len);
    return atk_send_frame(f, 3 + total);
}

/* A short-DDP frame arrived (link.c): give it to its socket. */
void ddp_deliver(const uint8_t *frame, int len)
{
    ddp_dgram d;
    int total;

    if (len < 3 + 5)
        return;
    total = ((frame[3] & 0x03) << 8) | frame[4];
    if (total < 5 || 3 + total > len)
        return;                         /* bad length */
    d.dst_node = frame[0];
    d.src_node = frame[1];
    d.dst_socket = frame[5];
    d.src_socket = frame[6];
    d.type = frame[7];
    d.data = frame + 8;
    d.len = total - 5;
    if (sockets[d.dst_socket].handler)
        sockets[d.dst_socket].handler(sockets[d.dst_socket].ctx, &d);
}
