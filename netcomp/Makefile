# netcomp: the visual6502 netlist compiled to C (ncc.py), node-exact against
# harness netsim.  Needs clang and python3 (stdlib only).
CC      := clang
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter
PYTHON  ?= python3 -I
HARNESS := ../harness
CPU6510 := ../cpu6510
JOBS    ?= 15

# Same SDK probe as harness/Makefile: some macOS SDKs ship a libSystem.tbd
# the installed linker cannot read; fall back to the newest SDK that links.
SDKS := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk 2>/dev/null | sort -rV)
LINK_OK := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null 2>/dev/null && echo yes)
ifneq ($(LINK_OK),yes)
  SYSROOT := $(firstword $(foreach s,$(SDKS),$(shell printf 'int main(void){return 0;}' | $(CC) -isysroot $(s) -x c - -o /dev/null 2>/dev/null && echo $(s))))
  ifneq ($(SYSROOT),)
    CFLAGS += -isysroot $(SYSROOT)
  endif
endif

INC  = -Isrc -I$(HARNESS)/src -I$(CPU6510)
GEN  = gen/ccpu_net.h gen/ccpu_net.inc gen/lcpu_net.h gen/lcpu_net.inc

all: nodestep lstep bench_ccpu bench_stats statetest cboard orderexp ldiverge ldebug

$(GEN): ncc.py tools/netlist.py tools/phases.py tools/levelize.py tools/lgen.py $(HARNESS)/gen/netlist6502.h
	$(PYTHON) ncc.py $(HARNESS)/gen/netlist6502.h gen

build:
	mkdir -p $@ logs

build/ccpu.o: src/ccpu.c src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/lcpu_stats.o: src/lcpu.c src/lcpu.h src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) -DCCPU_STATS $(INC) -c -o $@ $<
build/ccpu_stats.o: src/ccpu.c src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) -DCCPU_STATS $(INC) -c -o $@ $<
build/netsim.o: $(HARNESS)/src/netsim.c $(HARNESS)/src/netsim.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/netcpu.o: $(HARNESS)/src/netcpu.c $(HARNESS)/src/netcpu.h $(HARNESS)/gen/netlist6502.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/fastcpu.o: $(CPU6510)/fastcpu.c $(CPU6510)/fastcpu.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/nodestep.o: test/nodestep.c src/ccpu.h $(GEN) $(CPU6510)/lockstep.c | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/bench.o: test/bench.c src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

build/lcpu.o: src/lcpu.c src/lcpu.h src/ccpu.h $(GEN) gen/lcpu_net.h gen/lcpu_net.inc | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/lstep.o: test/lstep.c src/ccpu.h src/lcpu.h $(GEN) gen/lcpu_net.h $(CPU6510)/lockstep.c | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
lstep: build/lstep.o build/lcpu.o build/ccpu.o build/netsim.o build/netcpu.o build/fastcpu.o
	$(CC) $(CFLAGS) -o $@ $^

nodestep: build/nodestep.o build/ccpu.o build/netsim.o build/netcpu.o build/fastcpu.o
	$(CC) $(CFLAGS) -o $@ $^
bench_ccpu: build/bench.o build/ccpu.o build/lcpu.o build/netsim.o build/netcpu.o
	$(CC) $(CFLAGS) -o $@ $^
build/bench_stats.o: test/bench.c src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) -DCCPU_STATS $(INC) -c -o $@ $<
bench_stats: build/bench_stats.o build/ccpu_stats.o build/lcpu_stats.o build/netsim.o build/netcpu.o
	$(CC) $(CFLAGS) -o $@ $^

# ---------------------------------------------------------------- tests
# Every job runs ../cpu6510/lockstep.c's scenario through test/nodestep.c:
# netsim and ccpu compared node by node after every recalc (4 per cycle),
# fastcpu compared at bus level.  Lines: RESULT (bus, vs fastcpu) and NODES.
OPC  = opc-00 opc-20 opc-40 opc-60 opc-80 opc-A0 opc-C0 opc-E0
IRQ  = irq-seq irq-00 irq-40 irq-80 irq-C0
RDY  = rdy-00 rdy-40 rdy-80 rdy-C0
RND  = rnd-1 rnd-2 rnd-3 rnd-4 rnd-5 rnd-6 rnd-7 rnd-8
ALU  = alu-adc-00 alu-adc-01 alu-adc-10 alu-adc-11 alu-sbc-00 alu-sbc-01 alu-sbc-10 alu-sbc-11
TICK = tick-rnd tick-irq
TESTS = $(OPC) jam $(IRQ) $(RDY) $(ALU) reset warmreset $(RND) rom $(TICK)

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
logs/jam.log:       ARGS = jam
logs/reset.log:     ARGS = reset
logs/warmreset.log: ARGS = warmreset 1 2000
logs/rom.log:       ARGS = rom 6000000 $(HARNESS)/roms
logs/tick-rnd.log:  ARGS = random 9 1000000
logs/tick-rnd.log:  ENV  = NODESTEP_TICK=1
logs/tick-irq.log:  ARGS = irq 12 -1
logs/tick-irq.log:  ENV  = NODESTEP_TICK=1

logs/%.log: nodestep | build
	@echo "  running $(ENV) ./nodestep $(ARGS)"
	@$(ENV) ./nodestep $(ARGS) > $@ 2>&1 || true

logs/board.log: cboard | build
	@echo "  running ./cboard lockstep $(HARNESS)/roms 4000000"
	@./cboard lockstep $(HARNESS)/roms 4000000 > $@ 2>&1 || true

test: nodestep statetest cboard
	@rm -rf logs; mkdir -p logs
	@./statetest | tee logs/statetest.log
	@$(MAKE) --no-print-directory -j$(JOBS) $(TESTS:%=logs/%.log) logs/board.log
	@echo "---------------------------------------------------------------- results"
	@grep -h '^NODES' $(TESTS:%=logs/%.log)
	@grep -h '^RESULT' $(TESTS:%=logs/%.log) | grep -v warmreset | \
	  sed 's/.*cycles=\([0-9]*\) mismatches=\([0-9]*\).*/\1 \2/' | \
	  awk '{c+=$$1; m+=$$2} END {printf "bus level vs fastcpu: %d cycles, %d mismatches\n", c, m}'
	@grep -h '^NODES' $(TESTS:%=logs/%.log) | \
	  sed 's/.*halfcycles=\([0-9]*\) recalcs=\([0-9]*\) nodes_compared=\([0-9]*\) node_mismatches=\([0-9]*\).*bus_mismatches=\([0-9]*\).*/\1 \2 \3 \4 \5/' | \
	  awk '{h+=$$1; r+=$$2; n+=$$3; m+=$$4; b+=$$5} END {printf "TOTAL node level vs netsim: %d cycles (%d half cycles, %d recalcs), %.3g node comparisons, %d node mismatches, %d pin mismatches\n", h/2, h, r, n, m, b; \
	       exit (m > 0 || b > 0 || NR != $(words $(TESTS)))}'
	@grep -h '^RESULT board' logs/board.log
	@grep -q '^STATETEST OK' logs/statetest.log
	@grep -q '^RESULT board .*pin_mismatches=0 .*node_mismatches=0 ' logs/board.log
	@grep -h '^RESULT' $(filter-out logs/warmreset.log,$(TESTS:%=logs/%.log)) | grep -v ' mismatches=0 ' && exit 1 || true

build/statetest.o: test/statetest.c src/ccpu.h $(GEN) | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
statetest: build/statetest.o build/ccpu.o build/netsim.o build/netcpu.o
	$(CC) $(CFLAGS) -o $@ $^

bench: bench_ccpu bench_stats
	./bench_ccpu 2000000 $(HARNESS)/roms netsim
	./bench_stats 1000000 $(HARNESS)/roms

# ------------------------------------------------- order dependence of netsim
# build/ordersim.c = harness netsim.c with the nodes of each pass processed in
# another order (tools/mkordersim.py); exp/orderexp.c runs it against netsim.
OSIM_R = $(foreach f,create destroy drive release recalc stabilize get num_transistors stats state_size state_save state_load,-Dnetsim_$(f)=osim_$(f))
OCPU_R = $(foreach f,init free tick node_id node a x y s p pc ir transistors,-Dnetcpu_$(f)=ocpu_$(f))
build/ordersim.c: tools/mkordersim.py $(HARNESS)/src/netsim.c | build
	$(PYTHON) tools/mkordersim.py $(HARNESS)/src/netsim.c $@
build/ordersim.o: build/ordersim.c | build
	$(CC) $(CFLAGS) $(INC) $(OSIM_R) -c -o $@ $<
build/ocpu.o: $(HARNESS)/src/netcpu.c | build
	$(CC) $(CFLAGS) $(INC) $(OSIM_R) $(OCPU_R) -c -o $@ $<
build/orderexp.o: exp/orderexp.c | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
orderexp: build/orderexp.o build/ordersim.o build/ocpu.o build/netsim.o build/netcpu.o
	$(CC) $(CFLAGS) -o $@ $^
order: orderexp | build
	@./orderexp 1 400000 1 rom    | tee logs/order-1-rom.log
	@./orderexp 2 400000 2 rom    | tee logs/order-2-rom.log
	@./orderexp 1 200000 3 random | tee logs/order-1-rnd.log
	@./orderexp 2 200000 4 random | tee logs/order-2-rnd.log
	@NORESYNC=1 ./orderexp 1 200000 5 random | tee logs/order-1-rnd-noresync.log

# ------------------------------------- lcpu: the levelized (static-order) core
build/ldiverge.o: exp/ldiverge.c src/ccpu.h src/lcpu.h $(GEN) | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
ldiverge: build/ldiverge.o build/ccpu.o build/lcpu.o
	$(CC) $(CFLAGS) -o $@ $^
build/ldebug.o: exp/ldebug.c src/ccpu.h src/lcpu.h $(GEN) | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
ldebug: build/ldebug.o build/ccpu.o build/lcpu.o
	$(CC) $(CFLAGS) -o $@ $^
# every lcpu/netsim difference traced to its root: is it a floating node?
lcpu-roots: ldebug
	NDUMP=3000 ./ldebug
	$(PYTHON) tools/lroots.py build/ldebug.txt
lcpu-test: lstep ldiverge
	@./lstep random 1 300000 | grep -v '^lcpu mismatching' | tail -3 | tee logs/lcpu-rnd.log
	@LSTEP_NORESYNC=1 ./lstep rom 3000000 $(HARNESS)/roms | grep '^LCPU\|^RESULT' | tee logs/lcpu-rom-noresync.log
	@./ldiverge 20 100000 | tail -1 | tee logs/lcpu-diverge-all.log
	@DOC=1 ./ldiverge 20 300000 | tail -1 | tee logs/lcpu-diverge-documented.log

# ---------------------------------------------------- board-level lockstep
# Two whole C64 boards from the live harness sources: board A as the harness
# builds it (netlist CPU on netsim), board B = harness c64.c + port6510.c
# compiled again with renamed symbols and netcpu_init/tick/free bound to
# ccpu (test/ccpu_shim.c).  Depends on the harness layout (../vic2, ../sid,
# ../cpu6510 for fastcpu.h).
VIC2 = ../vic2
SIDD = ../sid
BINC = -Isrc -I$(HARNESS)/src -I$(VIC2) -I$(SIDD) -I$(CPU6510)
RENAME_PORT = $(foreach f,a free init init_fast p pc port_pins s tick x y,-Dm6510_$(f)=cm6510_$(f))
RENAME_NET  = -Dnetcpu_init=shim_netcpu_init -Dnetcpu_free=shim_netcpu_free -Dnetcpu_tick=shim_netcpu_tick
RENAME_C64  = $(foreach f,char_key free init key load_roms run_cycles run_frames screen_contains screen_text tick type typing use_fast_cpu vic_read,-Dc64_$(f)=cc64_$(f))
BOARD_SHARED = netsim netcpu port6510 pla cia sidpins c64 autostart
BOARD_OBJS = $(BOARD_SHARED:%=build/board/%.o) build/board/vic.o build/board/sid.o build/board/wav.o \
             build/board/fastcpu.o build/board/c_port6510.o build/board/c_c64.o build/board/ccpu_shim.o \
             build/ccpu.o build/board/cboard.o

cboard: $(BOARD_OBJS)
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
build/board/fastcpu.o: $(CPU6510)/fastcpu.c | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/c_port6510.o: $(HARNESS)/src/port6510.c | build/board
	$(CC) $(CFLAGS) $(BINC) $(RENAME_PORT) $(RENAME_NET) -c -o $@ $<
build/board/c_c64.o: $(HARNESS)/src/c64.c | build/board
	$(CC) $(CFLAGS) $(BINC) $(RENAME_PORT) $(RENAME_C64) -c -o $@ $<
build/board/ccpu_shim.o: test/ccpu_shim.c src/ccpu.h | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<
build/board/cboard.o: test/cboard.c src/ccpu.h | build/board
	$(CC) $(CFLAGS) $(BINC) -c -o $@ $<

INTRO = ../demo/minimalistic/prg/minimalistic_fig.prg
boardtest: cboard
	./cboard lockstep $(HARNESS)/roms 4000000
boardspeed: cboard
	./cboard speed $(HARNESS)/roms 3000000 $(INTRO)

# optional: clang profile-guided build of the bench (about +7 %)
LLVM_PROFDATA ?= $(shell command -v llvm-profdata || echo /opt/homebrew/opt/llvm/bin/llvm-profdata)
pgo: $(GEN) | build
	$(CC) $(CFLAGS) -fprofile-instr-generate $(INC) test/bench.c src/ccpu.c src/lcpu.c $(HARNESS)/src/netsim.c $(HARNESS)/src/netcpu.c -o build/bench_gen
	LLVM_PROFILE_FILE=build/bench.profraw ./build/bench_gen 300000 $(HARNESS)/roms > /dev/null
	$(LLVM_PROFDATA) merge -o build/bench.profdata build/bench.profraw
	$(CC) $(CFLAGS) -fprofile-instr-use=build/bench.profdata $(INC) test/bench.c src/ccpu.c src/lcpu.c $(HARNESS)/src/netsim.c $(HARNESS)/src/netcpu.c -o bench_pgo
	./bench_pgo 2000000 $(HARNESS)/roms

.PHONY: all clean test bench boardtest boardspeed order lcpu-test lcpu-roots pgo
clean:
	rm -rf build logs nodestep lstep bench_ccpu bench_stats statetest cboard orderexp ldiverge ldebug bench_pgo
