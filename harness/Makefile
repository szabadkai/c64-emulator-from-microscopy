# Headless C64 with a transistor-level (visual6502 netlist) 6502.
# Only clang + python3 (stdlib) are needed.
CC      := clang
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter
PYTHON  ?= python3 -I

# Some macOS setups ship an SDK whose libSystem.tbd the installed linker cannot
# read ("unknown architecture arm64e.x1").  Probe a trivial link and, if it
# fails, fall back to the newest SDK that links.
SDKS := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk 2>/dev/null | sort -rV)
LINK_OK := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null 2>/dev/null && echo yes)
ifneq ($(LINK_OK),yes)
  SYSROOT := $(firstword $(foreach s,$(SDKS),$(shell printf 'int main(void){return 0;}' | $(CC) -isysroot $(s) -x c - -o /dev/null 2>/dev/null && echo $(s))))
  ifneq ($(SYSROOT),)
    CFLAGS += -isysroot $(SYSROOT)
  endif
endif

# chips owned by other agents are built directly from their single source
VIC2 = ../vic2
SIDD = ../sid
FCPU = ../cpu6510
NCOMP = ../netcomp
CFLAGS += -Isrc -I$(VIC2) -I$(SIDD) -I$(FCPU) -I$(NCOMP)/src
LDLIBS = -lz -lm

SRCS = src/netsim.c src/netcpu.c src/port6510.c src/pla.c src/cia.c src/sidpins.c \
       src/c64.c src/autostart.c src/state.c src/png.c src/cputest.c src/ciatest.c src/main.c
OBJS = $(SRCS:src/%.c=build/%.o) build/ext_vic.o build/ext_sid.o build/ext_wav.o build/ext_fastcpu.o build/ext_ccpu.o
GEN  = gen/netlist6502.h gen/testrom.h gen/cputests.h
HDRS = $(wildcard src/*.h) $(VIC2)/vic.h $(SIDD)/sid.h $(SIDD)/wav.h $(FCPU)/fastcpu.h $(NCOMP)/src/ccpu.h

all: c64

c64: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# real-time window + audio front end (macOS, Swift/AppKit/AVFoundation)
LIBOBJS = $(filter-out build/main.o,$(OBJS)) build/player.o

build/libc64.a: $(LIBOBJS)
	rm -f $@ && ar rcs $@ $^

c64player: app/C64Player.swift src/player.h build/libc64.a
	swiftc -O -Xcc -I$(FCPU) -Xcc -I$(NCOMP)/src -import-objc-header src/player.h -o $@ app/C64Player.swift build/libc64.a -lz

build/%.o: src/%.c $(GEN) $(HDRS) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ext_vic.o: $(VIC2)/vic.c $(VIC2)/vic.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ext_sid.o: $(SIDD)/sid.c $(SIDD)/sid.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ext_wav.o: $(SIDD)/wav.c $(SIDD)/wav.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ext_fastcpu.o: $(FCPU)/fastcpu.c $(FCPU)/fastcpu.h src/netcpu.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

# compiled netlist: gen/ccpu_net.* come from ../netcomp/ncc.py (make -C ../netcomp)
build/ext_ccpu.o: $(NCOMP)/src/ccpu.c $(NCOMP)/src/ccpu.h $(NCOMP)/gen/ccpu_net.inc src/netcpu.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build gen out:
	mkdir -p $@

gen/netlist6502.h: tools/convert_netlist.py data/visual6502/segdefs.js data/visual6502/transdefs.js data/visual6502/nodenames.js | gen
	$(PYTHON) tools/convert_netlist.py data/visual6502 $@

gen/font.s: tools/mkfont.py | gen
	$(PYTHON) tools/mkfont.py $@

gen/testrom.h: asm/testrom.s gen/font.s tools/asm6502.py | gen build
	$(PYTHON) tools/asm6502.py rom asm/testrom.s build/testrom.bin E000 10000 --header $@ --name testrom_bin

gen/cputests.h: $(wildcard asm/cputests/*.s) tools/asm6502.py | gen
	$(PYTHON) tools/asm6502.py cputests $@ $(sort $(wildcard asm/cputests/*.s))

# fetch the netlist data (the only download this project needs)
data/visual6502/%.js:
	mkdir -p data/visual6502
	curl -sSfL -o $@ https://raw.githubusercontent.com/trebonian/visual6502/master/$*.js

test: c64 | out
	./c64 --cpu-test
	./c64 --cia-test
	./c64 --test-rom --frames 100 --screen-text --stats --screenshot out/test.png
	./c64 --rom-dir roms --cpu netlist --frames 300 --until-text READY. --screen-text --stats --screenshot out/basic_ready.png

bench: c64
	./c64 --cpu-bench 300000

clean:
	rm -rf build c64 gen/testrom.h gen/cputests.h gen/font.s

.PHONY: all test bench clean
