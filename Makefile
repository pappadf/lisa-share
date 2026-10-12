# lisa-share -- AppleTalk file sharing for the Lisa Office System, built with
# lisa-c (https://github.com/pappadf/lisa-c).  See README.md.
#
#   make                    build the tool's floppy -> build/lisashare.dc42
#   make driver             build the LocalTalk driver -> build/atalk.drv (SYSTEM.CD_ATALK)
#   make probe-llap         ... and the probe that installs it and pings the network
#   make probe-serialb      build the Serial B takeover probe (a desktop tool)
#   make run-probe-serialb  ... and run it on the LOS desktop in granny-smith;
#                           screenshots in build/emu-serialb/
#   make test               run every probe in granny-smith; PASS/FAIL from the results
#                           file each probe writes on its floppy
#   make serve              run it in granny-smith's web build: prints a link to open
#                           in a browser (lisa-c/emu/serve.sh)
#   make demo               a scripted session in granny-smith (build/demo/*.png)
#   make screenshots        ... and docs/*.png from it, at the Lisa's proportions
#   make toolchain          download lisa-c's prebuilt compiler (once)
#   make clean
#
# LISA_C is the lisa-c checkout: the submodule (git clone --recursive), or a
# lisa-c next to this directory.
LISA_C   ?= $(if $(wildcard lisa-c/Makefile),lisa-c,../lisa-c)
LISA_MAKE = $(MAKE) -C $(LISA_C)

all: tool

# The tool, Lisa Share (tool/), with the driver on its floppy.  It has no
# stationery: like the Calculator it is opened from its own icon, and its
# window is titled "Lisa Share".  The window is where the Desktop Manager
# puts a new pad's (2, 33, 716, 314), wide enough for the list view.
TOOLNUM  := 22
TOOLNAME := Lisa Share
TOOLOPTS := TOOLONLY=1 TOOLWINDOW=33,2,314,716
tool: build/lisashare.dc42
build/lisashare.dc42: tool/*.c tool/*.h tool/*.phrase lib/*.c lib/*.h build/atalk.drv
	@[ -f $(LISA_C)/Makefile ] || { echo "no lisa-c at $(LISA_C): git submodule update --init, or set LISA_C" >&2; exit 1; }
	$(LISA_MAKE) PROG=lisashare SRCDIR=$(CURDIR)/tool LIBDIRS='$(CURDIR)/lib' TOOLNUM=$(TOOLNUM) \
	    TOOLNAME='$(TOOLNAME)' TOOLFILES='SYSTEM.CD_ATALK=$(CURDIR)/build/atalk.drv' $(TOOLOPTS) tool
	mkdir -p build
	cp $(LISA_C)/build/lisashare-tool.dc42 $@

# make test-tool: in granny-smith, find the server, open the share, copy
# Hello.txt and paste it on the diskette, then copy the new document back to
# the server (tests/roundtrip.script).  The Mac file document's data fork
# must be the file, and so must the server's "Hello.txt 2".
test-tool: build/lisashare.dc42 build/afp-share.script
	@rm -rf build/emu-tool/saved.dc42 build/emu-tool/files && mkdir -p build/emu-tool/files
	@$(SHARE_RESET)
	@PRE=build/afp-share.script POST=tests/roundtrip.script SAVE=build/emu-tool/saved.dc42 TOOLONLY=1 \
	    $(LISA_C)/emu/run-los-tool.sh $< build/emu-tool > build/emu-tool.log 2>&1 || \
	    { echo "FAIL: tool (emulator; see build/emu-tool.log)"; exit 1; }
	@python3 -I $(LISA_C)/tools/lisadisk.py desktop build/emu-tool/saved.dc42 > build/emu-tool/desktop.txt
	@doc=$$(awk '$$2 == "document" && $$3 == "$(TOOLNUM)" && $$4 == "Hello.txt" {print $$1}' build/emu-tool/desktop.txt); \
	 python3 -I $(LISA_C)/tools/lisadisk.py extract build/emu-tool/saved.dc42 "{D$${doc}T$(TOOLNUM)}DATA" \
	     build/emu-tool/files/DATA > /dev/null 2>&1; \
	 if [ -n "$$doc" ] && cmp -s build/emu-tool/files/DATA build/afp-share/Hello.txt; then echo "PASS: tool (download)"; \
	 else echo "FAIL: tool (download; see build/emu-tool/*.png, desktop.txt)"; exit 1; fi
	@if cmp -s "build/afp-share/Hello.txt 2" build/afp-share/Hello.txt; then echo "PASS: tool (upload)"; \
	 else echo "FAIL: tool (upload; see build/emu-tool/*.png)"; exit 1; fi

# make test-bundle: a Lisa document (tool 20's "Aardvark Note", on a test
# floppy) to the server as "Aardvark Note.lisa" and back (tests/bundle.script);
# the new document's file must be the original's.
build/aardvark.txt:
	printf 'A note written on the Lisa.\n' > $@
build/lisashare-test.dc42: build/lisashare.dc42 build/aardvark.txt
	$(LISA_MAKE) PROG=lisashare SRCDIR=$(CURDIR)/tool LIBDIRS='$(CURDIR)/lib' TOOLNUM=$(TOOLNUM) \
	    TOOLNAME='$(TOOLNAME)' TOOLFILES='SYSTEM.CD_ATALK=$(CURDIR)/build/atalk.drv' $(TOOLOPTS) \
	    DISKARGS="--document 'Aardvark Note' 20 TEXT=$(CURDIR)/build/aardvark.txt" tool
	cp $(LISA_C)/build/lisashare-tool.dc42 $@
test-bundle: build/lisashare-test.dc42 build/afp-share.script
	@rm -rf build/emu-bundle/saved.dc42 build/emu-bundle/files
	@$(SHARE_RESET)
	@mkdir -p build/emu-bundle/files
	@PRE=build/afp-share.script POST=tests/bundle.script SAVE=build/emu-bundle/saved.dc42 TOOLONLY=1 \
	    $(LISA_C)/emu/run-los-tool.sh $< build/emu-bundle > build/emu-bundle.log 2>&1 || \
	    { echo "FAIL: bundle (emulator; see build/emu-bundle.log)"; exit 1; }
	@python3 -I $(LISA_C)/tools/lisadisk.py desktop build/emu-bundle/saved.dc42 > build/emu-bundle/desktop.txt
	@ids=$$(awk '$$2 == "document" && $$3 == "20" {print $$1}' build/emu-bundle/desktop.txt); \
	 new=$$(echo $$ids | awk '{print $$NF}'); \
	 python3 -I $(LISA_C)/tools/lisadisk.py extract build/emu-bundle/saved.dc42 "{D$${new}T20}TEXT" \
	     build/emu-bundle/files/TEXT > /dev/null 2>&1; \
	 if [ -f "build/afp-share/Aardvark Note.lisa/MANIFEST" ] && [ $$(echo $$ids | wc -w) = 2 ] && \
	    cmp -s build/emu-bundle/files/TEXT build/aardvark.txt; then echo "PASS: tool (Lisa document there and back)"; \
	 else echo "FAIL: tool (Lisa document; see build/emu-bundle/*.png, desktop.txt)"; exit 1; fi

# make test-convert: a long text (tools/longtext.py, many pages) from the
# server as a LisaWrite document, and
# back as text (tests/convert.script).  The document's file must be what
# tools/lwmake.py (checked against LisaWrite 3.1) makes of the text, and the
# server's new text file the text with CR line ends.
test-convert: build/lisashare.dc42 build/afp-share.script
	@rm -rf build/emu-convert/saved.dc42 build/emu-convert/files && mkdir -p build/emu-convert/files
	@$(SHARE_RESET)
	@python3 -I tools/longtext.py > "build/afp-share/A Long Text.txt"
	@PRE=build/afp-share.script POST=tests/convert.script SAVE=build/emu-convert/saved.dc42 TOOLONLY=1 \
	    $(LISA_C)/emu/run-los-tool.sh $< build/emu-convert > build/emu-convert.log 2>&1 || \
	    { echo "FAIL: convert (emulator; see build/emu-convert.log)"; exit 1; }
	@python3 -I $(LISA_C)/tools/lisadisk.py desktop build/emu-convert/saved.dc42 > build/emu-convert/desktop.txt
	@doc=$$(awk '$$2 == "document" && $$3 == "1" {print $$1}' build/emu-convert/desktop.txt); \
	 python3 -I $(LISA_C)/tools/lisadisk.py extract build/emu-convert/saved.dc42 "{D$${doc}T1}" \
	     build/emu-convert/files/lisawrite > /dev/null 2>&1; \
	 python3 -I tools/lwmake.py "build/afp-share/A Long Text.txt" build/emu-convert/files/expected; \
	 if [ -n "$$doc" ] && cmp -s build/emu-convert/files/lisawrite build/emu-convert/files/expected; then \
	     echo "PASS: tool (text to LisaWrite)"; \
	 else echo "FAIL: tool (text to LisaWrite; see build/emu-convert/)"; exit 1; fi
	@tr '\n' '\r' < "build/afp-share/A Long Text.txt" > build/emu-convert/files/expected.txt
	@if cmp -s "build/afp-share/A Long Text" build/emu-convert/files/expected.txt; then echo "PASS: tool (LisaWrite to text)"; \
	 else echo "FAIL: tool (LisaWrite to text; see build/emu-convert/)"; exit 1; fi

# The LocalTalk driver, SYSTEM.CD_ATALK (driver/)
driver: build/atalk.drv
build/atalk.drv: driver/*.c lib/atalk_api.h
	@[ -f $(LISA_C)/Makefile ] || { echo "no lisa-c at $(LISA_C): git submodule update --init, or set LISA_C" >&2; exit 1; }
	$(LISA_MAKE) PROG=atalk SRCDIR=$(CURDIR)/driver driver
	mkdir -p build
	cp $(LISA_C)/build/atalk.drv $@

# A probe is a desktop tool in probes/<name>/ (<name>.c, <name>.phrase).
PROBE_serialb_NUM  := 23
PROBE_serialb_NAME := Serial B Probe
PROBE_llap_NUM     := 25
PROBE_llap_NAME    := LocalTalk Probe
PROBE_llap_FILES   := SYSTEM.CD_ATALK=$(CURDIR)/build/atalk.drv
PROBE_nbp_NUM      := 26
PROBE_nbp_NAME     := NBP Probe
PROBE_nbp_FILES    := SYSTEM.CD_ATALK=$(CURDIR)/build/atalk.drv
PROBE_afp_NUM      := 27
PROBE_afp_NAME     := Server Probe
PROBE_afp_FILES    := SYSTEM.CD_ATALK=$(CURDIR)/build/atalk.drv
PROBE_paste_NUM    := 28
PROBE_paste_NAME   := Paste Probe
PROBE_paste_POST   := probes/paste/post.script
build/llap.dc42 build/nbp.dc42 build/afp.dc42: build/atalk.drv
# Every probe is built with the probe skeleton (probes/common) and the
# AppleTalk stack (lib).
PROBE_LIBDIRS := $(CURDIR)/probes/common $(CURDIR)/lib

probe-%: build/%.dc42 ;

build/%.dc42: probes/%/*.c probes/%/*.phrase probes/common/* lib/*.c lib/*.h
	@[ -f $(LISA_C)/Makefile ] || { echo "no lisa-c at $(LISA_C): git submodule update --init, or set LISA_C" >&2; exit 1; }
	$(LISA_MAKE) PROG=$* SRCDIR=$(CURDIR)/probes/$* LIBDIRS='$(PROBE_LIBDIRS)' TOOLNUM=$(PROBE_$*_NUM) \
	    TOOLNAME='$(PROBE_$*_NAME)' TOOLFILES='$(PROBE_$*_FILES)' tool
	mkdir -p build
	cp $(LISA_C)/build/$*-tool.dc42 $@

run-probe-%: build/%.dc42 $(PROBE_afp_PRE)
	$(SHARE_RESET)
	PRE='$(PROBE_$*_PRE)' POST='$(PROBE_$*_POST)' $(LISA_C)/emu/run-los-tool.sh $< build/emu-$*

# make test: run every probe on the LOS desktop in granny-smith, save its
# floppy, read the results file the probe wrote back on the host and look for
# the line that says it worked.  PASS/FAIL per probe.
PROBES := serialb llap nbp afp paste
PROBE_serialb_EXPECT := RESULT: Serial B detached and restored
PROBE_llap_EXPECT    := RESULT: echo reply received
PROBE_nbp_EXPECT     := RESULT: file server found
PROBE_afp_EXPECT     := RESULT: file read: Hello from the AppleShare server!
PROBE_paste_EXPECT   := RESULT: document created by paste
# The afp probe reads from a share published on granny-smith's network.
PROBE_afp_PRE        := build/afp-share.script
build/afp-share.script:
	echo 'appletalk.afp.volumes.add("Lisa Share", "$(CURDIR)/build/afp-share")' > $@
# Every test that uses the share starts from the same one: Hello.txt alone.
SHARE_RESET = rm -rf build/afp-share && mkdir -p build/afp-share && \
	printf 'Hello from the AppleShare server!\nA second line.\n' > build/afp-share/Hello.txt
test: $(addprefix test-,$(PROBES)) test-tool test-bundle test-convert
test-%: build/%.dc42 $(PROBE_afp_PRE)
	@rm -f build/emu-$*/saved.dc42 build/emu-$*/results.txt
	@$(SHARE_RESET)
	@PRE='$(PROBE_$*_PRE)' POST='$(PROBE_$*_POST)' SAVE=build/emu-$*/saved.dc42 $(LISA_C)/emu/run-los-tool.sh $< build/emu-$* > build/emu-$*.log 2>&1 || \
	    { echo "FAIL: $* (emulator; see build/emu-$*.log)"; exit 1; }
	@python3 -I $(LISA_C)/tools/lisadisk.py extract build/emu-$*/saved.dc42 \
	    $$(echo $* | tr a-z A-Z).RESULTS build/emu-$*/results.txt > /dev/null 2>&1 || true
	@if grep -qF '$(PROBE_$*_EXPECT)' build/emu-$*/results.txt 2>/dev/null; then echo "PASS: $*"; \
	 else echo "FAIL: $* (see build/emu-$*/results.txt and *.png)"; exit 1; fi

# The emulator: granny-smith's web build from GS_VERSION on has the Lisa's
# Serial B on its AppleTalk network.  The Office System disk: when you have no
# own copy (LOS_IMAGE), archive.org's LOS 3.1 with the seven Lisa
# applications, so that LisaWrite opens converted documents.  The web build's
# file server shares the browser's /opfs/shared folder as "Shared".
GS_VERSION ?= v0.9.4
LOS_APPS_URL := https://ia600806.us.archive.org/view_archive.php?archive=/22/items/apple-lisa-profile-hd-disk-images-for-lisaem-and-idle-lisa-office-system-3.1-lis/LisaEM_LOS3.1with7LisaApps.zip&file=lisaem-profile.dc42
serve: build/lisashare.dc42
	GS_VERSION=$(GS_VERSION) LOS_URL='$(LOS_APPS_URL)' $(LISA_C)/emu/serve.sh $<

# The demo needs an Office System with LisaWrite (LOS_IMAGE; see
# lisa-c/emu/env.sh) for its last screenshot.
demo: build/lisashare.dc42
	LISA_C=$(abspath $(LISA_C)) tests/demo.sh $< build/demo
screenshots: demo
	python3 -I tools/aspect.py build/demo/7-both.png docs/screenshot.png
	python3 -I tools/aspect.py build/demo/3-icons.png docs/screenshot2.png
	python3 -I tools/aspect.py build/demo/8-lisawrite.png docs/screenshot3.png

toolchain:
	$(MAKE) -C $(LISA_C) toolchain

clean:
	rm -rf build

.PHONY: all toolchain clean driver test tool test-tool test-bundle test-convert serve demo screenshots
.SECONDARY:
