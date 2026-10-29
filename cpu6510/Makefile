# fastcpu: behavioural 6502 core + lockstep validation against the netlist CPU.
# The netlist sources are compiled from ../harness (read-only, not copied).
CC      := clang
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter
HARNESS := ../harness
INC     := -I. -I$(HARNESS)/src
JOBS    ?= 12

# Same SDK probe as harness/Makefile: some macOS SDKs ship a libSystem.tbd the
# installed linker cannot read; fall back to the newest SDK that links.
SDKS := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk 2>/dev/null | sort -rV)
LINK_OK := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null 2>/dev/null && echo yes)
ifneq ($(LINK_OK),yes)
  SYSROOT := $(firstword $(foreach s,$(SDKS),$(shell printf 'int main(void){return 0;}' | $(CC) -isysroot $(s) -x c - -o /dev/null 2>/dev/null && echo $(s))))
  ifneq ($(SYSROOT),)
    CFLAGS += -isysroot $(SYSROOT)
  endif
endif

OBJS = build/fastcpu.o build/lockstep.o build/netsim.o build/netcpu.o

all: lockstep

lockstep: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

build/fastcpu.o: fastcpu.c fastcpu.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/lockstep.o: lockstep.c fastcpu.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/netsim.o: $(HARNESS)/src/netsim.c $(HARNESS)/src/netsim.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/netcpu.o: $(HARNESS)/src/netcpu.c $(HARNESS)/src/netcpu.h $(HARNESS)/gen/netlist6502.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

build logs:
	mkdir -p $@

# ---------------------------------------------------------------- tests
# Every job prints "RESULT <test> cycles=N mismatches=M"; the jobs run in
# parallel (JOBS=12), the summary adds them up and fails on any mismatch.
OPC  = opc-00 opc-20 opc-40 opc-60 opc-80 opc-A0 opc-C0 opc-E0
IRQ  = irq-seq irq-00 irq-40 irq-80 irq-C0
RDY  = rdy-00 rdy-40 rdy-80 rdy-C0
RND  = rnd-1 rnd-2 rnd-3 rnd-4 rnd-5 rnd-6 rnd-7 rnd-8
ALU  = alu-adc-00 alu-adc-01 alu-adc-10 alu-adc-11 alu-sbc-00 alu-sbc-01 alu-sbc-10 alu-sbc-11
TESTS = $(OPC) jam $(IRQ) $(RDY) $(ALU) reset $(RND) rom
INFO  = warmreset

logs/opc-00.log: ARGS = opcodes 1 400 0x00 0x1F
logs/opc-20.log: ARGS = opcodes 2 400 0x20 0x3F
logs/opc-40.log: ARGS = opcodes 3 400 0x40 0x5F
logs/opc-60.log: ARGS = opcodes 4 400 0x60 0x7F
logs/opc-80.log: ARGS = opcodes 5 400 0x80 0x9F
logs/opc-A0.log: ARGS = opcodes 6 400 0xA0 0xBF
logs/opc-C0.log: ARGS = opcodes 7 400 0xC0 0xDF
logs/opc-E0.log: ARGS = opcodes 8 400 0xE0 0xFF
logs/irq-seq.log: ARGS = irq 11 -1
logs/irq-00.log: ARGS = irq 21 0x00 0x3F
logs/irq-40.log: ARGS = irq 22 0x40 0x7F
logs/irq-80.log: ARGS = irq 23 0x80 0xBF
logs/irq-C0.log: ARGS = irq 24 0xC0 0xFF
logs/rdy-00.log: ARGS = rdy 31 4 0x00 0x3F
logs/rdy-40.log: ARGS = rdy 32 4 0x40 0x7F
logs/rdy-80.log: ARGS = rdy 33 4 0x80 0xBF
logs/rdy-C0.log: ARGS = rdy 34 4 0xC0 0xFF
logs/rnd-1.log: ARGS = random 1 3000000
logs/rnd-2.log: ARGS = random 2 3000000
logs/rnd-3.log: ARGS = random 3 3000000
logs/rnd-4.log: ARGS = random 4 3000000
logs/rnd-5.log: ARGS = random 5 3000000
logs/rnd-6.log: ARGS = random 6 3000000
logs/rnd-7.log: ARGS = random 7 3000000
logs/rnd-8.log: ARGS = random 8 3000000
logs/alu-adc-00.log: ARGS = alu adc 0 0
logs/alu-adc-01.log: ARGS = alu adc 0 1
logs/alu-adc-10.log: ARGS = alu adc 1 0
logs/alu-adc-11.log: ARGS = alu adc 1 1
logs/alu-sbc-00.log: ARGS = alu sbc 0 0
logs/alu-sbc-01.log: ARGS = alu sbc 0 1
logs/alu-sbc-10.log: ARGS = alu sbc 1 0
logs/alu-sbc-11.log: ARGS = alu sbc 1 1
logs/jam.log:    ARGS = jam
logs/reset.log:  ARGS = reset
logs/warmreset.log: ARGS = warmreset 1 2000
logs/rom.log:    ARGS = rom 6000000 $(HARNESS)/roms

logs/%.log: lockstep | logs
	@echo "  running ./lockstep $(ARGS)"
	@./lockstep $(ARGS) > $@ 2>&1 || true

test: lockstep
	@rm -rf logs; mkdir -p logs
	@$(MAKE) --no-print-directory -j$(JOBS) $(TESTS:%=logs/%.log) $(INFO:%=logs/%.log)
	@echo "---------------------------------------------------------------- results"
	@grep -h '^RESULT' $(TESTS:%=logs/%.log)
	@echo "info (approximate model, not a pass/fail criterion):"
	@grep -h '^warm resets' $(INFO:%=logs/%.log)
	@grep -h '^RESULT.*mismatches=' $(TESTS:%=logs/%.log) | \
	  sed 's/.*cycles=\([0-9]*\) mismatches=\([0-9]*\).*/\1 \2/' | \
	  awk '{c+=$$1; m+=$$2} END {printf "TOTAL: %d cycles compared in lockstep, %d mismatches\n", c, m; \
	       exit (m > 0 || NR != $(words $(TESTS)))}'

bench: lockstep
	./lockstep bench 200000000

# ---------------------------------------------------- board-level lockstep
# Two whole C64 boards built from the live harness sources: board 1 as the
# harness builds it (netlist), board 2 = harness c64.c + port6510.c compiled
# again with renamed symbols and netcpu_init/tick/free bound to fastcpu
# (fastshim.c).  Depends on the current harness layout (../vic2, ../sid).
VIC2 = ../vic2
SIDD = ../sid
BINC = -I. -I$(HARNESS)/src -I$(VIC2) -I$(SIDD)
RENAME_PORT = -Dm6510_init=fm6510_init -Dm6510_free=fm6510_free -Dm6510_tick=fm6510_tick \
              -Dm6510_port_pins=fm6510_port_pins
RENAME_NET  = -Dnetcpu_init=shim_netcpu_init -Dnetcpu_free=shim_netcpu_free -Dnetcpu_tick=shim_netcpu_tick
RENAME_C64  = $(foreach f,init free load_roms tick run_cycles run_frames vic_read screen_text \
                screen_contains key type typing inject_prg install_psid save_state load_state state_hash,-Dc64_$(f)=fc64_$(f))
BOARD_SHARED = netsim netcpu port6510 pla cia sidpins c64
BOARD_OBJS = $(BOARD_SHARED:%=build/board/%.o) build/board/vic.o build/board/sid.o build/board/wav.o \
             build/board/f_port6510.o build/board/f_c64.o build/board/fastshim.o build/board/fastcpu.o \
             build/board/boardstep.o

boardstep: $(BOARD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BOARD_OBJS) -lz -lm
build/board:
	mkdir -p $@
build/board/%.o: $(HARNESS)/src/%.c | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/vic.o: $(VIC2)/vic.c | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/sid.o: $(SIDD)/sid.c | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/wav.o: $(SIDD)/wav.c | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/f_port6510.o: $(HARNESS)/src/port6510.c | build/board
	$(CC) $(CFLAGS) $(BINC) $(RENAME_PORT) $(RENAME_NET) -c -o $@ $<
build/board/f_c64.o: $(HARNESS)/src/c64.c | build/board
	$(CC) $(CFLAGS) $(BINC) $(RENAME_PORT) $(RENAME_C64) -c -o $@ $<
build/board/fastshim.o: fastshim.c fastcpu.h | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/fastcpu.o: fastcpu.c fastcpu.h | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/boardstep.o: boardstep.c fastcpu.h | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<

boardtest: boardstep
	./boardstep $(HARNESS)/roms 4000000

clean:
	rm -rf build logs lockstep boardstep

.PHONY: all test bench boardtest clean
