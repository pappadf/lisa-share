#!/usr/bin/env bash
# A scripted Lisa Share session in granny-smith, for the screenshots in
# docs/: browse the demo share (demo/share), switch views, copy a text file
# as a LisaWrite document onto the diskette and open it in LisaWrite.
#
#   tests/demo.sh floppy.dc42 [out-dir]
#
# Uses lisa-c's emulator settings (LISA_C/emu/env.sh: gs-headless, the Lisa
# ROM and the Office System image) and its run-los-tool.sh, which boots the
# Office System, opens the diskette and opens the tool.  Screen positions are
# those of the Lisa Share diskette and window (the emulator is deterministic).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${LISA_C:-}" ]; then          # the submodule, else a lisa-c next to this project
    if [ -f "$HERE/../lisa-c/Makefile" ]; then LISA_C="$HERE/../lisa-c"; else LISA_C="$HERE/../../lisa-c"; fi
fi
OUT="$(realpath -m "${2:-$HERE/../build/demo}")"
rm -rf "$OUT" && mkdir -p "$OUT"

# The share, with dates from the Lisa's time (git keeps no file dates)
SHARE="$OUT/share"
cp -r "$HERE/../demo/share" "$SHARE"
touch -d "1983-01-19 09:30" "$SHARE/Lisa Brochure.txt"
touch -d "1984-01-24 11:05" "$SHARE/Price List.txt"
touch -d "1985-04-29 16:20" "$SHARE/Read Me.txt"
touch -d "1984-08-02 10:00" "$SHARE/Letters/"* "$SHARE/Letters"
touch -d "1983-10-31 14:45" "$SHARE/Recipes/"* "$SHARE/Recipes"
echo "appletalk.afp.volumes.add(\"Shared\", \"$SHARE\")" > "$OUT/pre.script"

S="$OUT/post.script"
emit() { echo "$*" >> "$S"; }
run() { emit "scheduler.run $1"; }
shot() { emit "machine.screen.save \"$OUT/$1.png\""; }
park() { emit "machine.adb.mouse.move 600 345 \"global\""; run 2000000; }   # the pointer out of the way
click() { emit "click($1, $2)"; run 5000000; }
dclick() { emit "dclick($1, $2)"; }
drag() {    # drag x1 y1 x2 y2: press, move there in steps, release
    emit "machine.adb.mouse.move $1 $2 \"global\""; run 2000000
    emit "machine.adb.mouse.click true"; run 3000000
    for i in 1 2 3 4 5 6; do
        emit "machine.adb.mouse.move $(( $1 + ($3 - $1) * i / 6 )) $(( $2 + ($4 - $2) * i / 6 )) \"global\""; run 1000000
    done
    run 2000000
    emit "machine.adb.mouse.click false"; run 10000000
}
menu() {    # menu title-x item-y [shot]: choose an item; with a name, a screenshot of the open menu
    emit "machine.adb.mouse.move $1 7 \"global\""; run 2000000
    emit "machine.adb.mouse.click true"; run 3000000
    emit "machine.adb.mouse.move $1 $2 \"global\""; run 3000000
    [ -n "${3:-}" ] && shot "$3"
    emit "machine.adb.mouse.click false"; run 3000000
}
# Menu titles: File/Print 95, Edit 168, Server 230, Housekeeping 310 (the
# Desktop Manager's Housekeeping: 250); items 15 pixels apart from y 23.
# Rows of the list view 16 pixels apart from y 60; the window's status
# line says what happened.
: > "$S"
menu 230 23; run 60000000                       # Server > Find Servers
click 100 60; menu 230 38; run 60000000         # open "Shared Folders"
click 100 60; menu 230 38; run 60000000         # open the volume "Shared"
park; shot 1-volume
menu 310 23 2-menu; run 10000000                # Housekeeping > Pictorial View
park; shot 3-icons
menu 310 38; run 10000000                       # Alphabetical View
click 100 108                                   # Read Me.txt
park; shot 4-list
menu 168 38; run 20000000                       # Edit > Copy as LisaWrite
menu 95 38; run 60000000                        # File/Print > Set Aside
click 400 200                                   # the diskette's window
menu 168 98; run 150000000                      # Edit > Paste
shot 5-pasted
# The diskette's window in list view, below; then Lisa Share above it
menu 250 68; run 20000000                       # Housekeeping > Alphabetical View
drag 628 283 714 160                            # its size control: wider, shorter
drag 450 31 450 186                             # its title bar: down
shot 6-disk-list
dclick 58 338; run 100000000                    # reopen Lisa Share (set aside: bottom left)
drag 705 306 714 168                            # its size control: the top half
run 20000000
park; shot 7-both
click 400 290                                   # the diskette's window
click 30 251                                    # Read Me
menu 95 83; run 400000000                       # File/Print > Open "Read Me": LisaWrite starts
park; shot 8-lisawrite
if [ -n "${EXPLORE:-}" ]; then
    menu 250 7 x-dm-menu                        # what the Desktop Manager's menu holds
fi
emit quit

PRE="$OUT/pre.script" POST="$S" TOOLONLY=1 "$LISA_C/emu/run-los-tool.sh" "$1" "$OUT/emu" > "$OUT/demo.log" 2>&1 || true
ls "$OUT"/*.png
