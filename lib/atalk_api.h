/* The interface of SYSTEM.CD_ATALK, the LocalTalk (LLAP) driver, as the
 * programs that use it see it.  Shared by the driver and its clients.
 *
 * The driver runs Serial B (SCC channel B) as a LocalTalk port.  Attach it
 * with CDMAKE at position (10, 2, 0), MOUNT "RS232B", then open
 * -RS232B-<anything>:
 *   - READ_DATA returns one LLAP frame: destination node, source node, LLAP
 *     type, then the data (no CRC).  It blocks until a frame arrives, unless
 *     reads are made non-blocking (ATK_SET_NONBLOCK), when it returns 0
 *     bytes if none is waiting.  Frames for this node and broadcasts are
 *     received; lapRTS, lapCTS, lapENQ and lapACK are handled by the driver.
 *   - WRITE_DATA sends one frame laid out the same way; the driver fills in
 *     the source node and runs the LLAP dialog (lapRTS/lapCTS for a directed
 *     frame, lapRTS to 255 for a broadcast).
 *   - DEVICE_CONTROL on -RS232B, dcversion 2, with one of the codes below.
 * The node address is acquired (lapENQ) when the device is first opened. */
#ifndef ATALK_API_H
#define ATALK_API_H

#define ATK_DRIVER_ID 0x41544C4BL       /* 'ATLK' */
#define ATK_DRIVER_NAME "ATALK"         /* SYSTEM.CD_ATALK */

#define LLAP_MAX_FRAME 603              /* 3-byte header + 600 bytes of data */
#define LLAP_BROADCAST 0xFF
#define LLAP_DDP_SHORT 1                /* LLAP types */
#define LLAP_DDP_LONG 2
#define LLAP_ENQ 0x81
#define LLAP_ACK 0x82
#define LLAP_RTS 0x84
#define LLAP_CTS 0x85

/* DEVICE_CONTROL codes (dccode) and their dcdata */
enum {
    ATK_STATUS = 1,         /* out: [0] node, [1] frames waiting, [2] frames received,
                               [3] frames dropped (queue full), [4] bad frames,
                               [5] frames sent, [6] sends failed, [7] lapCTS sent,
                               [8] lapACK sent, [9] lapENQ sent */
    ATK_ACQUIRE = 2,        /* in: [0] first node to try (0: any); out: [0] the node */
    ATK_SET_NONBLOCK = 3,   /* in: [0] 1 = reads return at once, 0 = reads wait */
    ATK_TIME = 4,           /* out: [0] the OS's microsecond timer (MICROTIMER) */
};

/* errors */
enum {
    ATK_E_NOSPACE = 610,    /* no room in sysglobal for the driver's buffers */
    ATK_E_FUNCTION = 656,   /* a function the driver does not do */
    ATK_E_CONTROL = 657,    /* an unknown DEVICE_CONTROL code */
    ATK_E_SEND = 658,       /* the frame could not be sent (no lapCTS, line busy) */
    ATK_E_BUSY = 659,       /* a read is already waiting */
    ATK_E_FRAME = 660,      /* a frame shorter than 3 or longer than 603 bytes */
};

#endif
