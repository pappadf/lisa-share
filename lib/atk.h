/* lisa-share's AppleTalk stack, for a desktop tool.
 *
 * It runs in the tool's process on top of the LocalTalk driver
 * SYSTEM.CD_ATALK (driver/, lib/atalk_api.h), which the tool carries on its
 * floppy: atk_open installs the driver at Serial B in place of the serial
 * driver, and atk_close puts the serial driver back.
 *
 * The stack is single-threaded and polled: atk_poll() reads the frames that
 * have arrived (the driver queues a few) and hands each to its DDP socket's
 * handler; the request/response calls (nbp_lookup, ...) poll until their
 * answer or their timeout.  A LocalTalk network without a router (network
 * number 0, short DDP headers) is assumed. */
#ifndef ATK_H
#define ATK_H
#include <stdint.h>
#include "atalk_api.h"

/* ---- the link ------------------------------------------------------------ */

/* Install and open the driver.  0, or an OS error; atk_failed_step() names
   the step that failed. */
int atk_open(void);
void atk_close(void);
const char *atk_failed_step(void);
uint8_t atk_node(void);
uint32_t atk_time_us(void);             /* the OS's microsecond timer */
int atk_status(int32_t data[10]);       /* ATK_STATUS counters */
int atk_control(int code, int32_t data[10]);    /* any DEVICE_CONTROL code (atalk_api.h) */
int atk_send_frame(const uint8_t *frame, int len);  /* a raw LLAP frame */
int atk_poll(void);                     /* handle waiting frames; returns how many */
void atk_wait_ms(int ms);               /* poll for ms milliseconds */

/* ---- DDP (Inside AppleTalk ch. 4) ------------------------------------------ */

enum { DDP_RTMP = 1, DDP_NBP = 2, DDP_ATP = 3, DDP_AEP = 4, DDP_ZIP = 6, DDP_ADSP = 7 };
enum { DDP_MAX_DATA = 595 };            /* 600 bytes of LLAP data less the 5-byte short header */

typedef struct ddp_dgram {
    uint8_t src_node, src_socket, dst_node, dst_socket, type;
    const uint8_t *data;
    int len;
} ddp_dgram;

typedef void (*ddp_handler)(void *ctx, const ddp_dgram *d);

/* Open a socket: `socket` 0 picks a free dynamic one (128-254).  Returns
   the socket number, or -1. */
int ddp_open(uint8_t socket, ddp_handler handler, void *ctx);
void ddp_close(uint8_t socket);
/* Send a datagram (dst_node 255: broadcast).  0, or an error. */
int ddp_send(uint8_t dst_node, uint8_t dst_socket, uint8_t src_socket, uint8_t type,
             const void *data, int len);

/* ---- NBP (Inside AppleTalk ch. 7) ------------------------------------------ */

typedef struct nbp_entity {
    char object[33], type[33], zone[33];
    uint16_t net;
    uint8_t node, socket, enumerator;
} nbp_entity;

/* Look up object:type@zone ("=" matches anything; zone "*" is this zone),
   broadcasting `tries` times, `interval_ms` apart.  Fills `out` with up to
   `max` distinct entities and returns how many. */
int nbp_lookup(const char *object, const char *type, const char *zone, nbp_entity *out, int max,
               int interval_ms, int tries);

/* ---- ATP (Inside AppleTalk ch. 9): the requesting end ---------------------- */

enum { ATP_MAX_DATA = 578, ATP_E_TIMEOUT = -1096 /* reqFailed */ };

/* A request that arrived on our ATP socket: control byte, TID, user bytes, data. */
typedef void (*atp_request_handler)(uint8_t node, uint8_t socket, uint8_t control, uint16_t tid,
                                    const uint8_t *user, const uint8_t *data, int len);

int atp_socket(void);                   /* our socket (opened on first use), or -1 */
void atp_set_request_handler(atp_request_handler handler);
/* A transaction: send the request (exactly-once if `xo`), collect up to 8
   response packets into `reply` and the first one's user bytes into
   `user_out`.  0, or ATP_E_TIMEOUT after `tries` x `interval_ms`. */
int atp_request(uint8_t node, uint8_t socket, int xo, const uint8_t user[4], const void *data, int len,
                uint8_t user_out[4], uint8_t *reply, int max, int *reply_len, int interval_ms, int tries);
int atp_send_request(uint8_t node, uint8_t socket, const uint8_t user[4], const void *data, int len);
/* One response packet (sequence number `seq`, `eom` on the last). */
int atp_send_response(uint8_t node, uint8_t socket, uint16_t tid, int seq, int eom, const uint8_t user[4],
                      const void *data, int len);

/* ---- ASP (Inside AppleTalk ch. 11): the workstation end -------------------- */

typedef struct asp_session {
    uint8_t node, sss, sid, open;       /* server node and session socket, session ID */
    uint16_t seq;                       /* the last command's sequence number */
    uint32_t last_sent_us;              /* for tickling */
    uint32_t server_time;               /* AFP: the server's clock, from afp_volumes */
} asp_session;

int asp_get_status(uint8_t node, uint8_t sls, uint8_t *buf, int max, int *len);
int asp_open(asp_session *s, uint8_t node, uint8_t sls);        /* 0 or an error */
/* A command: the command result (for AFP its result code), or an ATP error. */
int32_t asp_command(asp_session *s, const uint8_t *cmd, int len, uint8_t *reply, int max, int *reply_len);
/* A write command: the server asks for `data` (at most 8 x 578 bytes) with a
   WriteContinue, which the session answers while the command runs. */
int32_t asp_write(asp_session *s, const uint8_t *cmd, int len, const uint8_t *data, int dlen, uint8_t *reply,
                  int max, int *reply_len);
void asp_tickle(asp_session *s);
void asp_close(asp_session *s);

/* ---- AFP 2.0 (lib/atk_afp.c) ---------------------------------------------- */

enum { AFP_ROOT_PARENT = 1, AFP_ROOT = 2 };     /* directory IDs */
enum { AFP_EOF = -5009, AFP_OBJECT_NOT_FOUND = -5018, AFP_SESS_CLOSED = -5022 };

typedef struct afp_entry {
    char name[33];
    uint8_t is_dir;
    uint32_t id;                        /* directory ID or file number */
    uint16_t offspring;                 /* directories */
    uint32_t data_len, rsrc_len;        /* files */
    uint8_t finder[32];                 /* files: type at 0, creator at 4 */
    int32_t cdate, mdate;               /* created, modified: the server's clock (AFP time) */
} afp_entry;

int32_t afp_login(asp_session *s);      /* guest ("No User Authent") */
int32_t afp_logout(asp_session *s);
/* The server's volumes (FPGetSrvrParms); also sets s->server_time.  AFP
   times are seconds from 2000-01-01 by the server's clock. */
int32_t afp_volumes(asp_session *s, char names[][28], int max, int *count);
int32_t afp_open_vol(asp_session *s, const char *name, uint16_t *vol_id);
int32_t afp_close_vol(asp_session *s, uint16_t vol_id);
/* List a directory from the 1-based index `start`. */
int32_t afp_enumerate(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int start,
                      afp_entry *out, int max, int *count);
int32_t afp_open_fork(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int rsrc,
                      uint16_t *fork);
int32_t afp_read(asp_session *s, uint16_t fork, uint32_t offset, int count, uint8_t *buf, int *got);
int32_t afp_close_fork(asp_session *s, uint16_t fork);
enum { AFP_OBJECT_EXISTS = -5017 };
/* Create an empty file (fails with AFP_OBJECT_EXISTS if there is one). */
int32_t afp_create_file(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path);
int32_t afp_create_dir(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, uint32_t *new_id);
int32_t afp_set_finder_info(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path,
                            const uint8_t finder[32]);
int32_t afp_open_fork_write(asp_session *s, uint16_t vol_id, uint32_t dir_id, const char *path, int rsrc,
                            uint16_t *fork);
/* Write up to 4624 bytes at `offset`. */
int32_t afp_write(asp_session *s, uint16_t fork, uint32_t offset, const uint8_t *data, int len);

#endif
