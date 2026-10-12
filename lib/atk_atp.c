/* ATP, the requesting end (Inside AppleTalk, 2nd ed., ch. 9).
 *
 * An ATP packet is DDP type 3 with an 8-byte header: the control byte
 * (function in bits 7-6: 01 TReq, 10 TResp, 11 TRel; bit 5 XO, bit 4 EOM,
 * bit 3 STS; bits 2-0 the XO release timer), the bitmap (TReq: the response
 * packets wanted) or sequence number (TResp), the transaction ID and 4 user
 * bytes; then up to 578 bytes of data.
 *
 * One transaction at a time, from one socket (atp_socket()), which is also
 * the ASP workstation session socket: the request is sent, and re-sent with
 * the bitmap of the packets still missing every `interval_ms`, until every
 * response packet up to the one marked EOM has arrived.  An exactly-once
 * transaction is then released (TRel).  Requests arriving on the socket (a
 * server's ASP tickles and attentions) go to the handler set with
 * atp_set_request_handler. */
#include <string.h>
#include "atk.h"

enum { TREQ = 0x40, TRESP = 0x80, TREL = 0xC0, FUNC = 0xC0, XO = 0x20, EOM = 0x10, STS = 0x08 };

static struct {
    int socket;                         /* our socket, 0 until opened */
    uint16_t next_tid;
    atp_request_handler request_handler;
    /* the transaction in progress */
    int active;
    uint16_t tid;
    uint8_t node, peer_socket;
    uint8_t missing;                    /* bitmap of the response packets still wanted */
    uint8_t resend;                     /* the responder asked for a retransmission (STS) */
    uint8_t user[4];                    /* the first response packet's user bytes */
    uint16_t len[8];
    uint8_t data[8][ATP_MAX_DATA];
} atp;

static void input(void *ctx, const ddp_dgram *d)
{
    const uint8_t *p = d->data;
    int seq, len = d->len - 8;
    uint16_t tid;

    (void)ctx;
    if (d->type != DDP_ATP || d->len < 8)
        return;
    tid = (uint16_t)(p[2] << 8 | p[3]);
    if ((p[0] & FUNC) == TREQ) {        /* someone asks us something */
        if (atp.request_handler)
            atp.request_handler(d->src_node, d->src_socket, p[0], tid, p + 4, p + 8, len);
        return;
    }
    if ((p[0] & FUNC) != TRESP || !atp.active || tid != atp.tid || d->src_node != atp.node)
        return;
    seq = p[1];
    if (seq > 7 || !(atp.missing & (1 << seq)))
        return;                         /* a duplicate */
    if (seq == 0)
        memcpy(atp.user, p + 4, 4);
    if (len > ATP_MAX_DATA)
        len = ATP_MAX_DATA;
    memcpy(atp.data[seq], p + 8, (size_t)len);
    atp.len[seq] = (uint16_t)len;
    atp.missing &= (uint8_t)~(1 << seq);
    if (p[0] & EOM)                     /* nothing after this one */
        atp.missing &= (uint8_t)((1 << seq) - 1);
    if (p[0] & STS)
        atp.resend = 1;
}

int atp_socket(void)
{
    if (!atp.socket) {
        int s = ddp_open(0, input, 0);
        if (s < 0)
            return -1;
        atp.socket = s;
        atp.next_tid = (uint16_t)atk_time_us();
    }
    return atp.socket;
}

void atp_set_request_handler(atp_request_handler handler)
{
    atp.request_handler = handler;
}

static int send_packet(uint8_t node, uint8_t socket, uint8_t control, uint8_t bitmap, uint16_t tid,
                       const uint8_t user[4], const void *data, int len)
{
    static uint8_t pkt[8 + ATP_MAX_DATA];

    if (len > ATP_MAX_DATA)
        return ATK_E_FRAME;
    pkt[0] = control;
    pkt[1] = bitmap;
    pkt[2] = (uint8_t)(tid >> 8);
    pkt[3] = (uint8_t)tid;
    if (user)
        memcpy(pkt + 4, user, 4);
    else
        memset(pkt + 4, 0, 4);
    if (len > 0)
        memcpy(pkt + 8, data, (size_t)len);
    return ddp_send(node, socket, (uint8_t)atp.socket, DDP_ATP, pkt, 8 + len);
}

int atp_send_request(uint8_t node, uint8_t socket, const uint8_t user[4], const void *data, int len)
{
    if (atp_socket() < 0)
        return ATK_E_FUNCTION;
    return send_packet(node, socket, TREQ, 1, atp.next_tid++, user, data, len);
}

int atp_send_response(uint8_t node, uint8_t socket, uint16_t tid, int seq, int eom, const uint8_t user[4],
                      const void *data, int len)
{
    return send_packet(node, socket, (uint8_t)(TRESP | (eom ? EOM : 0)), (uint8_t)seq, tid, user, data, len);
}

int atp_request(uint8_t node, uint8_t socket, int xo, const uint8_t user[4], const void *data, int len,
                uint8_t user_out[4], uint8_t *reply, int max, int *reply_len, int interval_ms, int tries)
{
    int t, i, n, err = ATP_E_TIMEOUT;
    uint32_t start;

    if (atp_socket() < 0)
        return ATK_E_FUNCTION;
    atp.active = 1;
    atp.tid = atp.next_tid++;
    atp.node = node;
    atp.peer_socket = socket;
    atp.missing = 0xFF;
    memset(atp.user, 0, 4);
    memset(atp.len, 0, sizeof atp.len);
    for (t = 0; t < tries && atp.missing; t++) {
        atp.resend = 0;
        send_packet(node, socket, (uint8_t)(TREQ | (xo ? XO : 0)), atp.missing, atp.tid, user, data, len);
        start = atk_time_us();
        while (atp.missing && !atp.resend && atk_time_us() - start < (uint32_t)interval_ms * 1000u)
            atk_poll();
    }
    atp.active = 0;
    if (!atp.missing) {
        if (xo)                         /* release the responder's copy */
            send_packet(node, socket, TREL, 0, atp.tid, 0, 0, 0);
        n = 0;
        for (i = 0; i < 8 && (atp.missing & (1 << i)) == 0 && atp.len[i] + n <= max; i++) {
            memcpy(reply + n, atp.data[i], atp.len[i]);
            n += atp.len[i];
            if (atp.len[i] < ATP_MAX_DATA)
                break;                  /* the last packet of the response */
        }
        *reply_len = n;
        if (user_out)
            memcpy(user_out, atp.user, 4);
        err = 0;
    }
    return err;
}
