/* ASP, the workstation end (Inside AppleTalk, 2nd ed., ch. 11).
 *
 * ASP's header lives in the ATP user bytes: [SPFunction, session ID,
 * sequence number (2)], or for OpenSess [4, WSS, version (2)] with the reply
 * [SSS, session ID, error (2)].  A command is an exactly-once ATP request to
 * the server session socket (SSS); the first response packet's user bytes
 * carry the 4-byte command result (for AFP, its result code), and the reply
 * data is the response packets' data, up to 8 x 578 bytes.  Both ends tickle
 * (SPFunction 5, one-way) to keep the session alive: a server closes a
 * session after two minutes of silence; asp_tickle sends ours, and is called
 * from asp_command when 30 seconds have passed. */
#include <string.h>
#include "atk.h"

enum { CLOSE_SESS = 1, COMMAND = 2, GET_STATUS = 3, OPEN_SESS = 4, TICKLE = 5, WRITE = 6,
       WRITE_CONTINUE = 7, ATTENTION = 8 };
enum { INTERVAL_MS = 500, TRIES = 10, TICKLE_US = 30000000 };

/* The data of the write in progress, for the server's WriteContinue */
static struct {
    const uint8_t *data;
    int len;
    uint8_t sid;
} pending;

/* A request from the server on our session socket: WriteContinue (send the
   write data, up to the buffer size it offers, in response packets),
   Attention (acknowledge it) or Tickle (nothing to answer). */
static void server_request(uint8_t node, uint8_t socket, uint8_t control, uint16_t tid, const uint8_t *user,
                           const uint8_t *data, int len)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    int room, n, seq, chunk;

    (void)control;
    if (user[0] == WRITE_CONTINUE && pending.data && user[1] == pending.sid) {
        room = len >= 2 ? (data[0] << 8 | data[1]) : 8 * ATP_MAX_DATA;
        n = pending.len < room ? pending.len : room;
        for (seq = 0; seq < 8; seq++) {
            chunk = n - seq * ATP_MAX_DATA;
            if (chunk > ATP_MAX_DATA)
                chunk = ATP_MAX_DATA;
            if (chunk < 0)
                chunk = 0;
            atp_send_response(node, socket, tid, seq, (seq + 1) * ATP_MAX_DATA >= n, zero,
                              pending.data + seq * ATP_MAX_DATA, chunk);
            if ((seq + 1) * ATP_MAX_DATA >= n)
                break;
        }
    } else if (user[0] == ATTENTION) {
        atp_send_response(node, socket, tid, 0, 1, zero, 0, 0);
    }
}

int asp_get_status(uint8_t node, uint8_t sls, uint8_t *buf, int max, int *len)
{
    uint8_t user[4] = {GET_STATUS, 0, 0, 0};

    return atp_request(node, sls, 0, user, 0, 0, 0, buf, max, len, INTERVAL_MS, TRIES);
}

int asp_open(asp_session *s, uint8_t node, uint8_t sls)
{
    uint8_t user[4], reply_user[4], dummy[8];
    int wss = atp_socket(), len, err;

    memset(s, 0, sizeof *s);
    if (wss < 0)
        return ATK_E_FUNCTION;
    user[0] = OPEN_SESS;
    user[1] = (uint8_t)wss;
    user[2] = 0x01;                     /* ASP version 1.0 */
    user[3] = 0x00;
    err = atp_request(node, sls, 1, user, 0, 0, reply_user, dummy, sizeof dummy, &len, INTERVAL_MS, TRIES);
    if (err)
        return err;
    err = (int16_t)(reply_user[2] << 8 | reply_user[3]);
    if (err)
        return err;                     /* e.g. -1071 ServerBusy, -1066 BadVersNum */
    atp_set_request_handler(server_request);
    s->node = node;
    s->sss = reply_user[0];
    s->sid = reply_user[1];
    s->seq = 0;
    s->open = 1;
    s->last_sent_us = atk_time_us();
    return 0;
}

void asp_tickle(asp_session *s)
{
    uint8_t user[4] = {TICKLE, s->sid, 0, 0};

    if (!s->open)
        return;
    atp_send_request(s->node, s->sss, user, 0, 0);
    s->last_sent_us = atk_time_us();
}

int32_t asp_command(asp_session *s, const uint8_t *cmd, int len, uint8_t *reply, int max, int *reply_len)
{
    uint8_t user[4], result[4];
    int err;

    *reply_len = 0;
    if (!s->open)
        return ATK_E_FUNCTION;
    if (atk_time_us() - s->last_sent_us > TICKLE_US)
        asp_tickle(s);
    s->seq++;
    user[0] = COMMAND;
    user[1] = s->sid;
    user[2] = (uint8_t)(s->seq >> 8);
    user[3] = (uint8_t)s->seq;
    err = atp_request(s->node, s->sss, 1, user, cmd, len, result, reply, max, reply_len, INTERVAL_MS, TRIES);
    s->last_sent_us = atk_time_us();
    if (err)
        return err;
    return (int32_t)((uint32_t)result[0] << 24 | (uint32_t)result[1] << 16 | (uint32_t)result[2] << 8 |
                     result[3]);
}

int32_t asp_write(asp_session *s, const uint8_t *cmd, int len, const uint8_t *data, int dlen, uint8_t *reply,
                  int max, int *reply_len)
{
    uint8_t user[4], result[4];
    int err;

    *reply_len = 0;
    if (!s->open)
        return ATK_E_FUNCTION;
    s->seq++;
    user[0] = WRITE;
    user[1] = s->sid;
    user[2] = (uint8_t)(s->seq >> 8);
    user[3] = (uint8_t)s->seq;
    pending.data = data;
    pending.len = dlen;
    pending.sid = s->sid;
    err = atp_request(s->node, s->sss, 1, user, cmd, len, result, reply, max, reply_len, INTERVAL_MS, TRIES);
    pending.data = 0;
    s->last_sent_us = atk_time_us();
    if (err)
        return err;
    return (int32_t)((uint32_t)result[0] << 24 | (uint32_t)result[1] << 16 | (uint32_t)result[2] << 8 |
                     result[3]);
}

void asp_close(asp_session *s)
{
    uint8_t user[4] = {CLOSE_SESS, s->sid, 0, 0}, dummy[8];
    int len;

    if (!s->open)
        return;
    atp_request(s->node, s->sss, 1, user, 0, 0, 0, dummy, sizeof dummy, &len, INTERVAL_MS, 3);
    s->open = 0;
}
