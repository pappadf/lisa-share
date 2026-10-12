# lisa-share architecture

This design comes from reading the Lisa OS source that the Computer History
Museum released. Source references are to that release
(`Lisa_Source/LISA_OS/OS/...`, `APPS/APDM/...` for the Desktop Manager, `DM`
below).

## Goal and non-goals

The goal is to exchange files easily between a Lisa running the Office System
and AppleShare servers and Macs, using one tool from a floppy.

These are deliberately out of scope:

- **AFP volumes mounted in the LOS file system.** The OS has no virtual file
  system layer. Its only per-device switch is block versus sequential device,
  and the `remotedev` flag and `netState` pointer were never finished. Also,
  the Desktop Manager does not list directories: it shows its own
  `{!CATALOG}` database of LOS documents. A remote file system would need a
  rebuilt OS and changes to the Desktop Manager, and Mac files still would not
  appear on the desktop.
- **A network disk** (a block device backed by an image file on the server).
  It would work without OS changes, but it does not help with exchanging
  files: the files would still be inside a Lisa file system.

## Components

```
 Lisa desktop (Desktop Manager)
      ▲  Clipboard icon references + Filer fcCopy events
      │
 lisa-share tool (C, one process)
   ├─ browser window, Mac file documents, converters
   ├─ AFP client · ASP · ATP · NBP · DDP
   └─ timer son process (Delay_Time loop, sends events)
      ▲  READ_DATA / WRITE_DATA (one frame each), DEVICE_CONTROL
      │
 SYSTEM.CD_ATALK  (configurable driver, child of the SCC at Serial B)
   └─ interrupt core: SDLC receive, lapRTS→lapCTS, lapENQ→lapACK, frame ring
```

### The driver, `SYSTEM.CD_ATALK`

- **Why it must be a driver.** LocalTalk has to answer an RTS within about
  200 µs and catch every frame addressed to it. LOS switches processes only at
  system calls (`source-SCHED`, `source-PROCASM`), and a user-mode process can
  neither mask interrupts nor own an interrupt vector. So the timing-critical
  work has to be a configurable driver running at interrupt level.
- **Hardware.** Serial B (SCC channel B) is wired for AppleBus:
  - RS-422 drivers;
  - a 3.6864 MHz oscillator on RTxCB;
  - the hardware manual's baud table has an AppleBus row, x1 clock with
    TC = 6, which gives exactly 230.4 kbit/s;
  - the boot ROM sets port B up "for Applebus".

  Channel A has none of this.
- **Channel ownership.** The SCC is a built-in parent driver at slot 10, and
  its two channels are children (`SOURCE-GENIO` `dattach`). The interrupt
  front end, RSINT in `source-mover`, routes channel-B interrupts to whichever
  driver is attached there. The stock serial driver keeps channel A.
- **Constraints** (`SOURCE-CD` `LOAD_DRIVER`, `source-DS3`):
  - the driver is one code segment, mapped read-only, with no writable
    globals;
  - A5 is the OS's;
  - its state lives in a `GETSPACE` control block;
  - kernel services come only from the driver jump table: requests, alarms,
    `INTSOFF`/`INTSON`, `MICROTIMER`, and so on;
  - a driver wakes a process only by completing an I/O request.
- **Interrupt core**, cycle-counted assembly:
  - stay in a polled loop for the whole RTS / CTS / data exchange, as the Mac
    MPP driver does;
  - use hardware address search for our node and broadcast;
  - queue frames in a preallocated ring;
  - hand off to level 1 with a 0-delay alarm, so copying to the user buffer
    and completing the request happen outside the critical section.
- **Interface** to the tool (`driver/atalk_api.h`):
  - `READ_DATA` returns one frame;
  - `WRITE_DATA` sends one frame, with line sense, backoff and the RTS/CTS
    exchange;
  - `DEVICE_CONTROL` handles node address, statistics and non-blocking mode.
- **Implementation status** (`driver/atalk.c`, written in C): it follows
  *Inside AppleTalk* appendix B. Reception is interrupt-driven. The handler
  reads the frame, answers lapRTS with lapCTS and takes the data frame, and
  answers lapENQ with lapACK. A waiting `READ_DATA` is completed from the
  interrupt (`FREEZE_SEG` on the pinned buffer). Sending runs in the writer's
  process with interrupt level 6 masked. The `llap` probe passes in
  granny-smith. Real-hardware timing is untested; the receive path may need
  assembly.
- **Emulator compatibility.** granny-smith's SCC passes SDLC frames whole, on
  channel B. It ends a frame on WR0 command 3, "reset Tx underrun/EOM", and
  appends two dummy CRC bytes to received frames. The driver should use the
  same register sequence as the Mac's MPP driver.

### Loading the driver from the tool (no installer)

This is proven by `probes/serialb` (the serial driver out and back) and by
lisa-c's `examples/drvtest` (a C driver installed and used the same way).

1. Copy the driver file from the floppy to `-<boot volume>-SYSTEM.CD_ATALK`.
   `LOAD_DRIVER` only loads from the boot volume. It recognizes a driver by
   its object-file contents, so a plain file copy works. The name does not
   start with `{`, so it is invisible on the desktop.
2. `CDINFO(10,2,0)` to save the Serial B driver's info.
3. `CDKILL(10,2,0)` (this also unmounts the device), then `CDMAKE(10,2,0)`
   with our driver's info (`cd_drvrname` = `ATALK`, our own `cd_driverid`,
   `cd_devt` = seqdev).
4. `MOUNT` the device (`RS232B`): a device attached with `CDMAKE` starts
   unmounted, and the file system opens only mounted devices. Open it as
   `-RS232B-<any name>`: a sequential device needs a dummy file name.
5. On quit, `CDKILL` ours, then `CDMAKE` with the saved info and `MOUNT`.

If the tool crashes, Serial B stays on AppleTalk until the next reboot.

### Protocol stack (in the tool, `lib/`)

Implemented: DDP with short headers, NBP lookup, ATP requests (exactly-once
with release), ASP (GetStatus, OpenSession, Command, Tickle, CloseSession) and
the AFP 2.0 calls for reading (FPLogin as guest, FPGetSrvrParms, FPOpenVol,
FPEnumerate, FPOpenFork, FPRead, FPCloseFork, FPCloseVol, FPLogout). The
stack is polled from the tool's process, which needs no son processes for
now. Reads from the driver are non-blocking; time comes from the driver
(`ATK_TIME`, the OS's microsecond timer); a request polls until its answer or
its timeout.

The original plan, kept for when a background process is needed:


- **Layers:** DDP (short headers on a single network; long headers when a
  router is present), NBP lookup, ATP with exactly-once transactions, ASP,
  and an AFP 2.0 client with guest login first.
- **Two-process split.** I/O to the driver is synchronous, so the tool keeps a
  son process that blocks in `READ_DATA` and passes frames on. The son lives
  as long as the tool, because sons die with their father.
- **Timers.** Timed events are disabled in this kernel: `Send_Event_Chn` with
  an interval returns error 1199. Retransmission timers therefore come from a
  timer son that loops on `Delay_Time` and sends events, the same pattern the
  Clock uses.
- **The foreground tool must keep making OS calls**, through `GetEvent`,
  `LetOthersRun` and I/O, so that the son processes get to run.

### Desktop integration: Clipboard icon references

LibFC's `StartIconRef`/`AddIconRef`/`EndIconRef`/`ReadIconRefs` put
references to desktop objects on the Clipboard. The DM handles them in both
directions:

- **Import.** The tool copies a reference with kind, toolID, name and a
  private `diskName`. The user pastes onto a disk or folder. The DM's
  `PasteRef` (`APDM-DESK2`) adds a catalog entry for a new document of that
  toolID. Because the reference is foreign, it then sends the tool an
  **`fcCopy`** event with the new document's file prefix, and the tool writes
  the document's files there.
- **Export.** The DM's Copy of selected icons (`ReferenceIcons`) puts
  references whose `diskName` is the document's file prefix, `-vol-{DnTm}`.
  The tool's Paste reads them and uploads the files.

### Sessions that expire

A server closes an ASP session after two minutes without tickles. A tool
whose window is inactive is blocked in `GetEvent` and cannot tickle, so this
happens whenever the user works elsewhere for a while. The browser therefore
reconnects when a call fails with SessClosed or an ATP timeout. It opens a new
session, logs in again, reopens the volume (AFP 2 directory IDs are fixed), and
retries the call once.

### Mac file documents and explicit conversion

Implemented (`tool/macfile.c`): the files under the document's prefix are
`DATA` (data fork), `RSRC` (resource fork, if not empty) and `INFO` (magic
`LSMF`, version, Mac name, Finder info, fork lengths, server and volume).

- **Copy as-is** creates a document owned by the lisa-share tool. Under its
  `{DnTm}` prefix it stores the data fork, the resource fork and the Finder
  info (type/creator, flags, dates). The DM copies, moves and sets it aside
  like any other document. Copying it back to a server restores the original
  file byte for byte.
- **Opening a Mac file document** shows its info and **Convert to…**, listing
  the converters available for its type.
- **The browser looks like a Desktop Manager window** (`tool/view.c`), so
  that the source of a copy (a server folder) and its destination (a Lisa
  disk or folder) look alike. The views, positions, fonts and sort orders are
  taken from APDM-DESK (DrawList, DrawRow, DrawTheIcon, SortContents).
  - Housekeeping has Pictorial, Alphabetical and Chronological View.
  - The icons are characters of the icon fonts, with FilerComm's codes
    (LIBFC InitIcons). Its `iconData`/`tinyData` tables are filled only in
    the Desktop Manager's process, so view.c keeps its own copy.
  - Servers show as computers, volumes as disks, `.lisa` bundles as
    documents.
  - Dates are the server's own times. A Mac client shifts them by the
    difference between the server's clock and its own; the Lisa's clock
    stops at 1995, so that would be wrong. AFP counts from 2000, but
    granny-smith's server counts from 1904; the server's clock tells which.
- **The browser offers both actions on every file:** *Copy* and *Copy &
  convert to…*. Outbound, it offers *Copy as-is* (the raw LOS document) and
  *Export as…*.
- **Converters operate on local Mac file documents**, so they can be
  developed without the network. The LOS application sources (LisaWrite =
  APLW, LisaDraw = APLD, LisaCalc = APLC) specify the native formats.
- **LisaWrite (implemented, `tool/lisawrite.c`).** A LisaWrite document (tool 1)
  is one file named exactly the document's prefix, `{D<id>T1}`, in the shared
  formatted-file format (LIBSU UnitFf, magic `$CCAA`, version 5).
  - It is a leader page, then 512-byte text pages. Each text page holds its
    characters, a run table (format offset << 9 | character index) ending in
    the "vanish" run at index 251, and a trailer of paragraph distances.
  - The writer uses only the standard format (97), so documents take
    LisaWrite's defaults.
  - A LisaWrite document is made the same way as a Mac file document: a
    Clipboard reference of tool 1 that the tool fills on fcCopy.
  - The reader takes the body text, preferring `{D..T1}TEMP` (the state a
    suspend writes).

## Testing

- **granny-smith** has an AppleTalk network inside the emulator, with an
  AFP 2.x server that publishes host directories (`appletalk.afp.volumes.add`)
  and stores Mac metadata in AppleDouble sidecar files.
- **Still needed:** the Lisa machine has to be connected to that network, as
  `src/machines/compact/plus.c` does with `atalk_conn_new`.
- **Determinism.** Tests are deterministic: screenshots are compared byte for
  byte, and the AFP server's statistics can be asserted.
- **Limits.** The emulator cannot check microsecond timing, which needs a
  real Lisa.

## Risks

1. **Timing on a real 5 MHz Lisa.** SCC accesses use the E-clock (about
   2–3.5 µs each). The RTS→CTS turnaround is estimated at 60–100 µs against
   the 200 µs gap: tight.
2. **Long interrupt-masked windows.** The receive loop holds interrupt level
   6 for a whole frame, up to about 22 ms. That can make Serial A overrun at
   high baud rates and delay the mouse and keyboard.
3. **Port B transmit enable.** It is probably RTS, but the polarity needs
   confirming on the schematic.
4. **Running from the floppy.** Whether a tool can run straight from the
   floppy, or has to be installed onto the hard disk first.
