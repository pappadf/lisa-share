/* SYSTEM.CD_ATALK: LocalTalk (LLAP) on the Lisa's Serial B.
 *
 * A LOS configurable driver (lisa-c, runtime/lisa_driver.h), attached as the
 * child of the SCC at channel B in place of the serial driver.  It runs the
 * channel as LocalTalk -- SDLC, FM0, 230.4 kbit/s -- and implements the
 * LocalTalk Link Access Protocol (Inside AppleTalk, 2nd ed., chapter 1 and
 * appendix B): dynamic node ID assignment with lapENQ/lapACK, the lapRTS /
 * lapCTS dialog for directed frames, the lapRTS-to-255 broadcast dialog, and
 * carrier sense with random backoff.  Clients read and write whole frames
 * (lib/atalk_api.h).
 *
 * Clocks (Lisa Hardware Manual, sec. 2.5.2 and 6.4): channel B's RTxC is a
 * 3.6864 MHz oscillator, 16 x 230.4 kHz.  The receiver is clocked by the
 * DPLL in FM mode from RTxC; the transmitter by the baud-rate generator from
 * RTxC with time constant 6, 3.6864 MHz / (2 x (6 + 2)) = 230.4 kHz.  The
 * RS-422 line driver of port B is enabled by RTS, as on the Macintosh.
 *
 * Reception is interrupt-driven: the SCC interrupts on a frame's first byte
 * (WR1: receive interrupt on first character or special condition), and the
 * handler reads the whole frame, answers lapRTS with lapCTS and then takes
 * the data frame that follows, answers lapENQ for our node with lapACK, and
 * queues data frames for READ_DATA.  Transmission runs in the writer's
 * process with the SCC's interrupt level masked, polling the chip.
 *
 * Timing: this is C.  It is meant to be correct first; whether a 5 MHz Lisa
 * answers a lapRTS within the 200 us interframe gap is checked on hardware
 * later, and the receive path may then move to assembly.  granny-smith's SCC
 * passes frames whole, so the emulator does not test that timing. */
#include "lisa_driver.h"
#include "../lib/atalk_api.h"

/* SCC channel B in the OS's I/O space (iospacemmu 126 = $FC0000; the
   serial driver's RSBASE is $FCD201: control, then data 4 bytes on) */
#define SCC_CTL  ((volatile uint8_t *)0xFCD201)
#define SCC_DATA ((volatile uint8_t *)0xFCD205)

#define NFRAMES 4                       /* received frames queued for READ_DATA */
#define FRAME_BUF (LLAP_MAX_FRAME + 3)  /* a frame, its two CRC bytes, and one spare */
#define WR5_IDLE 0x61                   /* Tx 8 bits/char, CRC enable */
#define WR5_SEND 0x6B                   /* ... with Tx enable and RTS (the line driver) */
#define WR3_RX   0xDD                   /* Rx 8 bits, enter hunt, CRC, address search, enable */
#define SPIN_BYTE 3000                  /* polls of RR0 before giving up on a frame's next byte */
#define SPIN_FRAME 6000                 /* ... on the frame that must follow (data after lapCTS) */
#define ENQ_TRIES 20                    /* lapENQs for one node ID (Inside AppleTalk: wksTries) */
#define MAX_ATTEMPTS 32                 /* deferrals or collisions before a send fails */

typedef struct atk_cb {
    uint8_t node;                       /* our node ID; 0 until acquired */
    uint8_t acquired;                   /* the node ID was verified with lapENQ */
    uint8_t adr_in_use;                 /* a lapACK for our node ID came back */
    uint8_t nonblock;                   /* READ_DATA returns at once when nothing waits */
    uint32_t seed;                      /* random numbers: node IDs, backoff */
    reqblk *read_req;                   /* the READ_DATA waiting for a frame */
    uint16_t rx_head, rx_tail;          /* queued frames: [rx_tail, rx_head) */
    uint16_t rx_len[NFRAMES];
    int32_t stat[10];                   /* ATK_STATUS counters [2..9] */
    uint8_t tmp[FRAME_BUF];             /* the frame being received */
    uint8_t txf[FRAME_BUF];             /* the frame being sent */
    uint8_t ring[NFRAMES][FRAME_BUF];
} atk_cb;

enum { S_RECEIVED = 2, S_DROPPED, S_BAD, S_SENT, S_FAILED, S_CTS, S_ACK, S_ENQ };

/* ---- the SCC ------------------------------------------------------------- */

static inline void wr(int reg, uint8_t value)
{
    *SCC_CTL = (uint8_t)reg;            /* point (registers 8-15 set Point High) */
    *SCC_CTL = value;
}

static inline uint8_t rd(int reg)
{
    *SCC_CTL = (uint8_t)reg;
    return *SCC_CTL;
}

static inline uint8_t rr0(void)
{
    return *SCC_CTL;                    /* the pointer rests at 0 */
}

/* Program channel B for LocalTalk (Zilog SCC UM table 4-11, SDLC mode). */
static void scc_init(uint8_t node)
{
    wr(9, 0x4A);                        /* reset channel B; keep MIE and No Vector (shared) */
    wr(4, 0x20);                        /* x1 clock, SDLC */
    wr(10, 0xE0);                       /* CRC preset to ones, FM0 */
    wr(6, node);                        /* address search: our node (and 255) */
    wr(7, 0x7E);                        /* the SDLC flag */
    wr(11, 0xF0);                       /* RTxC oscillator; Rx clock = DPLL, Tx clock = BRG */
    wr(12, 6);                          /* BRG time constant: 230.4 kHz from 3.6864 MHz */
    wr(13, 0);
    wr(14, 0x01);                       /* BRG from RTxC, enabled */
    wr(14, 0xA1);                       /* DPLL source: RTxC */
    wr(14, 0xC1);                       /* DPLL: FM mode */
    wr(14, 0x21);                       /* DPLL: enter search */
    wr(15, 0x00);                       /* no external/status interrupts */
    wr(5, WR5_IDLE);
    wr(3, WR3_RX);
    *SCC_CTL = 0x10;                    /* reset external/status interrupts (twice) */
    *SCC_CTL = 0x10;
    wr(1, 0x08);                        /* Rx interrupt on first character or special condition */
}

static void scc_off(void)
{
    wr(1, 0x00);
    wr(3, 0x00);
    wr(5, 0x00);
    wr(9, 0x4A);                        /* reset channel B for the next driver */
}

/* Send one frame: the SCC adds the flags and the CRC (Tx underrun). */
static void send_frame(const uint8_t *f, int len)
{
    int i, spin;

    wr(5, WR5_SEND);                    /* line driver on, transmitter on */
    *SCC_CTL = 0x80;                    /* reset Tx CRC generator */
    for (i = 0; i < len; i++) {
        for (spin = 0; !(rr0() & 0x04) && spin < SPIN_BYTE; spin++)
            ;                           /* Tx buffer empty */
        *SCC_DATA = f[i];
    }
    *SCC_CTL = 0xC0;                    /* reset Tx underrun/EOM: CRC and flag follow the last byte */
    for (spin = 0; !(rr0() & 0x40) && spin < SPIN_BYTE; spin++)
        ;                               /* the underrun: the CRC is going out */
    for (spin = 0; spin < 40; spin++)   /* about three byte times for CRC and flag */
        (void)rr0();
    wr(5, WR5_IDLE);                    /* transmitter and line driver off */
}

/* Read the frame whose first byte is available.  Returns its length without
   the CRC, or -1 for a bad frame (timeout, CRC error, overrun, size). */
static int read_frame(uint8_t *buf)
{
    int n = 0, spin;
    uint8_t r1;

    for (;;) {
        for (spin = 0; !(rr0() & 0x01); spin++)
            if (spin > SPIN_BYTE)
                goto bad;
        r1 = rd(1);                     /* status of the byte about to be read */
        if (n < FRAME_BUF)
            buf[n] = *SCC_DATA;
        else
            (void)*SCC_DATA;
        n++;
        if (r1 & 0x80)                  /* end of frame: that was the last CRC byte */
            break;
    }
    *SCC_CTL = 0x30;                    /* error reset: clears end of frame */
    wr(3, WR3_RX);                      /* hunt for the next frame */
    if ((r1 & 0x60) || n < 3 + 2 || n > LLAP_MAX_FRAME + 2)
        return -1;                      /* CRC error or overrun, or a bad size */
    return n - 2;

bad:
    *SCC_CTL = 0x30;
    wr(3, WR3_RX);
    return -1;
}

/* Wait (by polling) for a frame to start, then read it. */
static int spin_frame(uint8_t *buf, int spins)
{
    int spin;

    for (spin = 0; !(rr0() & 0x01); spin++)
        if (spin > spins)
            return 0;
    return read_frame(buf);
}

/* ---- received frames ------------------------------------------------------ */

static uint8_t random8(atk_cb *c)
{
    c->seed = c->seed * 1103515245u + 12345u;
    return (uint8_t)(c->seed >> 16);
}

/* Complete the waiting READ_DATA with the oldest queued frame, if both exist. */
static void complete_read(atk_cb *c)
{
    reqblk *req = c->read_req;
    seq_extend *ext;
    absptr buf;
    int16_t err;
    int32_t n, i;
    uint8_t *f;

    if (!req || c->rx_head == c->rx_tail)
        return;
    ext = (seq_extend *)req->req_extent;
    FREEZE_SEG(&err, ext->buff_rdb_ptr, ext->buff_offset, (absptr)req, &buf);
    if (err > 0)
        return;                         /* the buffer cannot be reached now; the next frame retries */
    f = c->ring[c->rx_tail % NFRAMES];
    n = c->rx_len[c->rx_tail % NFRAMES];
    if (n > ext->num_bytes)
        n = ext->num_bytes;
    for (i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = f[i];
    UNFREEZE_SEG(ext->buff_rdb_ptr);
    c->rx_tail++;
    c->read_req = 0;
    ext->xfer_count = n;
    ADJ_IO_CNT(0, ext->buff_rdb_ptr);
    UNBLK_REQ(req, 1);
}

static void queue_frame(atk_cb *c, int len)
{
    int i;
    uint8_t *f;

    if ((uint16_t)(c->rx_head - c->rx_tail) >= NFRAMES) {
        c->stat[S_DROPPED]++;
        return;
    }
    f = c->ring[c->rx_head % NFRAMES];
    for (i = 0; i < len; i++)
        f[i] = c->tmp[i];
    c->rx_len[c->rx_head % NFRAMES] = (uint16_t)len;
    c->rx_head++;
    c->stat[S_RECEIVED]++;
    complete_read(c);
}

/* A frame has been read into c->tmp: the LLAP receive side. */
static void handle_frame(atk_cb *c, int len)
{
    uint8_t *f = c->tmp, ctl[3];
    uint8_t type;

    if (len < 3) {
        c->stat[S_BAD]++;
        return;
    }
    type = f[2];
    if (type < 0x80) {                  /* data */
        if (f[0] == LLAP_BROADCAST || (c->node && f[0] == c->node))
            queue_frame(c, len);
        return;
    }
    switch (type) {
    case LLAP_RTS:                      /* to us: clear it, then take the data frame */
        if (c->node && f[0] == c->node) {
            ctl[0] = f[1];
            ctl[1] = c->node;
            ctl[2] = LLAP_CTS;
            send_frame(ctl, 3);
            c->stat[S_CTS]++;
            len = spin_frame(c->tmp, SPIN_FRAME);
            if (len >= 3 && c->tmp[2] < 0x80)
                queue_frame(c, len);
            else if (len != 0)
                c->stat[S_BAD]++;
        }
        break;                          /* an RTS to 255 announces a broadcast: it follows */
    case LLAP_ENQ:                      /* someone wants our node ID */
        if (c->acquired && f[0] == c->node) {
            ctl[0] = ctl[1] = c->node;
            ctl[2] = LLAP_ACK;
            send_frame(ctl, 3);
            c->stat[S_ACK]++;
        }
        break;
    case LLAP_ACK:                      /* the node ID we are trying is taken */
        if (f[0] == c->node)
            c->adr_in_use = 1;
        break;
    default:                            /* a stray lapCTS */
        break;
    }
}

/* The interrupt: frames are waiting in the receiver. */
static void interrupt(atk_cb *c)
{
    int len;

    if (c) {
        while (rr0() & 0x01) {
            len = read_frame(c->tmp);
            if (len < 0) {
                c->stat[S_BAD]++;
                break;
            }
            handle_frame(c, len);
        }
    } else {                            /* not initialized yet: drain and forget */
        while (rr0() & 0x01)
            (void)*SCC_DATA;
    }
    *SCC_CTL = 0x30;                    /* error reset (a latched end of frame) */
    *SCC_CTL = 0x28;                    /* reset Tx interrupt pending */
    *SCC_CTL = 0x10;                    /* reset external/status interrupts */
    *SCC_CTL = 0x38;                    /* reset highest interrupt under service */
}

/* ---- sending (process level, SCC interrupts masked) ----------------------- */

/* Poll for `us` microseconds; frames that arrive meanwhile are handled.
   Returns 1 if the line stayed quiet throughout, 0 if a frame was seen. */
static int quiet_for(atk_cb *c, int32_t us)
{
    int32_t start = MICROTIMER();
    int len;

    while (MICROTIMER() - start < us) {
        if (rr0() & 0x01) {
            len = read_frame(c->tmp);
            if (len >= 0)
                handle_frame(c, len);
            return 0;
        }
        if (!(rr0() & 0x10))            /* out of hunt: carrier on the line */
            return 0;
    }
    return 1;
}

/* Carrier sense with backoff: wait for the interdialog gap (400 us) plus a
   random number of 100 us slots below `backoff`. */
static int line_free(atk_cb *c, int backoff)
{
    return quiet_for(c, 400 + (int32_t)(random8(c) % backoff) * 100);
}

/* Wait up to `us` for a frame and read it into c->tmp; 0 if none came. */
static int wait_frame(atk_cb *c, int32_t us)
{
    int32_t start = MICROTIMER();

    while (MICROTIMER() - start < us)
        if (rr0() & 0x01)
            return read_frame(c->tmp);
    return 0;
}

/* Send the frame in c->txf with the LLAP dialog.  0 or an error. */
static int16_t send_data(atk_cb *c, int len)
{
    uint8_t dst = c->txf[0], rts[3];
    int attempt, backoff = 2, n;

    c->txf[1] = c->node;
    rts[0] = dst;
    rts[1] = c->node;
    rts[2] = LLAP_RTS;
    for (attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (!line_free(c, backoff)) {   /* deferred */
            if (backoff < 2)
                backoff = 2;
            continue;
        }
        send_frame(rts, 3);
        if (dst == LLAP_BROADCAST) {
            if (quiet_for(c, 200)) {    /* nobody objected within the interframe gap */
                send_frame(c->txf, len);
                c->stat[S_SENT]++;
                return 0;
            }
        } else {
            n = wait_frame(c, 400);
            if (n == 3 && c->tmp[2] == LLAP_CTS && c->tmp[0] == c->node && c->tmp[1] == dst) {
                send_frame(c->txf, len);
                c->stat[S_SENT]++;
                return 0;
            }
            if (n > 0)
                handle_frame(c, n);
        }
        if (backoff < 16)               /* assume a collision: back off further */
            backoff *= 2;
    }
    c->stat[S_FAILED]++;
    return ATK_E_SEND;
}

/* Dynamic node ID assignment (Inside AppleTalk, appendix B, AcquireAddress):
   try a node ID with lapENQs; a lapACK means it is taken, so pick another. */
static uint8_t acquire(atk_cb *c, uint8_t hint)
{
    uint8_t node = hint ? hint : (uint8_t)(random8(c) % 127 + 1), enq[3];
    int i, n;

    c->acquired = 0;
    for (;;) {
        c->node = node;
        wr(6, node);
        c->adr_in_use = 0;
        for (i = 0; i < ENQ_TRIES && !c->adr_in_use; i++) {
            if (!line_free(c, 2))
                continue;
            enq[0] = enq[1] = node;
            enq[2] = LLAP_ENQ;
            send_frame(enq, 3);
            c->stat[S_ENQ]++;
            n = wait_frame(c, 400);
            if (n > 0)
                handle_frame(c, n);
        }
        if (!c->adr_in_use)
            break;
        do
            node = (uint8_t)(random8(c) % 127 + 1);
        while (node == c->node);
    }
    c->acquired = 1;
    return node;
}

/* ---- the driver's functions ------------------------------------------------ */

static int16_t do_init(devrec *dev)
{
    absptr addr;
    atk_cb *c;
    int16_t level;

    if (!GETSPACE(sizeof(atk_cb), B_SYSGLOBAL, &addr))
        return ATK_E_NOSPACE;
    c = (atk_cb *)addr;
    __builtin_memset(c, 0, sizeof *c);
    c->seed = (uint32_t)MICROTIMER();
    INTSOFF(rsints, &level);
    scc_init(0);
    dev->cb_addr = addr;
    acquire(c, 0);
    INTSON(level);
    return 0;
}

static void finish(reqblk *req, int32_t n, int16_t err)
{
    seq_extend *ext = (seq_extend *)req->req_extent;

    ext->xfer_count = n;
    if (err)
        req->hard_error = err;
    ADJ_IO_CNT(0, ext->buff_rdb_ptr);
    UNBLK_REQ(req, err == 0);
}

static int16_t do_down(devrec *dev)
{
    atk_cb *c = (atk_cb *)dev->cb_addr;
    int16_t level;

    INTSOFF(rsints, &level);
    scc_off();
    dev->cb_addr = 0;
    INTSON(level);
    if (c) {
        if (c->read_req)
            finish(c->read_req, 0, ATK_E_FUNCTION);
        RELSPACE((absptr)c, B_SYSGLOBAL);
    }
    return 0;
}

static int16_t do_seqio(atk_cb *c, reqblk *req)
{
    seq_extend *ext = (seq_extend *)req->req_extent;
    absptr buf;
    int16_t err, level;
    int32_t i, len = ext->num_bytes;

    if (ext->read_flag) {
        INTSOFF(rsints, &level);
        if (c->read_req) {
            INTSON(level);
            finish(req, 0, ATK_E_BUSY);
        } else if (c->rx_head == c->rx_tail && c->nonblock) {
            INTSON(level);
            finish(req, 0, 0);
        } else {
            c->read_req = req;
            complete_read(c);           /* at once if a frame waits; else from the interrupt */
            INTSON(level);
        }
        return 0;
    }

    if (len < 3 || len > LLAP_MAX_FRAME) {
        finish(req, 0, ATK_E_FRAME);
        return 0;
    }
    FREEZE_SEG(&err, ext->buff_rdb_ptr, ext->buff_offset, (absptr)req, &buf);
    if (err > 0) {
        finish(req, 0, err);
        return 0;
    }
    for (i = 0; i < len; i++)
        c->txf[i] = ((uint8_t *)buf)[i];
    UNFREEZE_SEG(ext->buff_rdb_ptr);
    INTSOFF(rsints, &level);
    err = send_data(c, (int)len);
    INTSON(level);
    finish(req, err ? 0 : len, err);
    return 0;
}

static int16_t do_control(atk_cb *c, dc_rec *dc)
{
    int16_t level;
    int i;

    switch (dc->dcode) {
    case ATK_STATUS:
        INTSOFF(rsints, &level);
        for (i = 2; i < 10; i++)
            dc->ar10[i] = c->stat[i];
        dc->ar10[0] = c->node;
        dc->ar10[1] = (uint16_t)(c->rx_head - c->rx_tail);
        INTSON(level);
        return 0;
    case ATK_ACQUIRE:
        INTSOFF(rsints, &level);
        dc->ar10[0] = acquire(c, (uint8_t)dc->ar10[0]);
        INTSON(level);
        return 0;
    case ATK_TIME:
        dc->ar10[0] = MICROTIMER();
        return 0;
    case ATK_SET_NONBLOCK:
        INTSOFF(rsints, &level);
        c->nonblock = dc->ar10[0] != 0;
        if (c->nonblock && c->read_req && c->rx_head == c->rx_tail) {
            reqblk *req = c->read_req;
            c->read_req = 0;
            INTSON(level);
            finish(req, 0, 0);
            return 0;
        }
        INTSON(level);
        return 0;
    default:
        return ATK_E_CONTROL;
    }
}

pascal int16_t DRIVER(params *p)
{
    atk_cb *c = (atk_cb *)p->configptr->cb_addr;

    switch (p->fnctn_code) {
    case dinterrupt: interrupt(c); return 0;
    case dinit:      return do_init(p->configptr);
    case ddown:      return do_down(p->configptr);
    case seqio:      return c ? do_seqio(c, p->req) : ATK_E_FUNCTION;
    case dcontrol:   return c ? do_control(c, (dc_rec *)p->parptr) : ATK_E_FUNCTION;
    case ddiscon:
        if (c && c->read_req) {         /* closed with a read waiting */
            reqblk *req = c->read_req;
            c->read_req = 0;
            finish(req, 0, ATK_E_FUNCTION);
        }
        return 0;
    case dunattach:  p->still_inuse = 0; return 0;
    case dattach:    return 0;
    default:         return ATK_E_FUNCTION;
    }
}
