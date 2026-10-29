# fastcpu: a behavioural 6510 core that matches the netlist cycle for cycle

`fastcpu` is a hand-written NMOS 6502 core for fast development runs of the
C64 harness. It has the same pin interface as the transistor-level netlist CPU
(`harness/src/netcpu.c`) and produces the same bus cycles: address, R/W, SYNC
and written data, every cycle. The netlist is the reference. Every rule below
was measured on it and is checked in lockstep.

* **Speed:** about 345 M cycles/s on an Apple M5 Pro (350× a real PAL C64). The
  netlist CPU runs at roughly 60–100 k cycles/s.
* **Lockstep:** `make test` compares 58.1 M cycles against the netlist with
  **0 mismatches**. A separate board-level run uses two complete harness C64s
  (real VIC badlines/BA, CIAs, keyboard) and also shows 0 mismatches.
* **Own code only.** No third-party emulator, test suite or binary was used.
  The helper scripts were throwaway probes in Python stdlib and C.

## Files

| file | what |
|---|---|
| `fastcpu.h`, `fastcpu.c` | the core (750 lines; one `switch` over opcode×step) |
| `lockstep.c` | lockstep test driver plus the `bench` speed test |
| `boardstep.c`, `fastshim.c` | board-level lockstep built from the live harness sources (`make boardtest`) |
| `Makefile` | `make` builds `lockstep`; `make test` runs the suite; `make bench`; `make boardtest` |

The netlist sources are compiled straight from `../harness/src` and
`../harness/gen`. Nothing there is copied or modified.

## Interface

```c
#include "fastcpu.h"          /* includes netcpu.h for the CPU_* pin bits */
fastcpu_t cpu;
fastcpu_init(&cpu);           /* power-up, /RES held: same state as netcpu_init() */
pins = fastcpu_tick(&cpu, pins);   /* one PHI2 cycle, exactly like netcpu_tick() */
fastcpu_regs_t r = fastcpu_regs(&cpu);   /* PC A X Y S P (P: bit5=1, bit4=0, like netcpu_p) */
fastcpu_pc(&cpu); fastcpu_a(&cpu); ... fastcpu_p(&cpu); fastcpu_ir(&cpu); fastcpu_jammed(&cpu);
fastcpu_set_regs(&cpu, &r);   /* e.g. to copy another core's power-up values */
```

Pin semantics are those of `netcpu.h`:

* **Inputs.** `D0..D7` carry the data of the *previous* cycle's read.
  `IRQ`/`NMI`/`RES` (set = asserted) and `RDY` (set = high) apply to *this*
  cycle. RDY low halts this cycle only if it is a read.
* **Outputs.** `A0..A15`, `RW` (set = read), `SYNC`, and `D0..D7` on writes.
  All other bits pass through unchanged, as in netcpu.
* **Power-up.** `fastcpu_init` sets PC=$00FF A=$FF X=$44 Y=$00 S=$C4 P=$A0.
  These are the values the current netsim settles to in `netcpu_init`. If the
  netsim's power-up settling changes, copy the netlist's values with
  `fastcpu_set_regs`, as `lockstep.c` does.
* **Plain data.** `fastcpu_t` has no pointers, so `memcpy` is a complete save
  state.

## What is implemented

* **All 256 opcodes:** the 151 documented ones, all undocumented ones (LAX,
  SAX, DCP, ISC, SLO, RLA, SRE, RRA, ANC, ALR, ARR, SBX, SBC $EB, LAS, XAA,
  LAX #, SHA, SHX, SHY, TAS, and every NOP variant with its operand/dummy
  reads), and the 12 JAMs.
* **Every bus cycle:**
  * dummy reads (implied, PC, stack, page-crossing reads)
  * read-modify-write double writes (old value, then new)
  * abs,X / abs,Y / (zp),Y penalty reads
  * branch timing: 2 cycles not taken, 3 taken, 4 with page cross, including
    the dummy reads
  * JMP ($xxFF) page wrap
  * zero-page wrap
* **Decimal mode** behaves as on NMOS for ADC, SBC, ARR, RRA and ISC. ADC and
  SBC are checked exhaustively in lockstep.
* **Interrupts:**
  * IRQ, NMI, BRK and RESET all go through the shared 7-cycle sequence.
  * Each instruction polls in its last cycle, seeing the lines as they were in
    the penultimate cycle.
  * Branch quirk: a taken branch without page cross does not poll again. A
    page-crossing branch polls twice, and the results are ORed.
  * CLI, SEI and PLP latency, and RTI's immediate effect.
  * An NMI hijacks BRK or IRQ until the P push. An NMI that arrives too late is
    lost or delayed by one instruction, depending on /NMI in the vector-high
    cycle.
* **RDY** halts read cycles only. The halted cycle is repeated, and the
  netlist's side effects during the halt are reproduced (see below).
* **RESET** matches the netlist's power-up hold (including its odd address
  pattern), any hold length, and reset out of a JAM. A warm reset (RES asserted
  while running) is approximate; see Known limits.

## Behaviour measured on the netlist

These rules were found with throwaway probes on the netlist, implemented, and
then confirmed by the lockstep suite.

**Interrupt polling (`POLL`).**

* The step that outputs an instruction's last cycle does
  `intr |= (irq_line_last_cycle && !I) | nmi_edge`. The I flag is applied at
  poll time; the lines are sampled at the end of every cycle.
* `intr` is cleared at every opcode fetch. If it was set, the fetched opcode
  becomes BRK (ir=0) and PC is not incremented.
* A 1-cycle IRQ pulse in the penultimate cycle is enough to trigger.

**NMI around an IRQ or BRK sequence** (the sequence's cycles are C1–C7):

* An NMI edge seen by C4 selects $FFFA: it hijacks the sequence. B stays set
  for BRK.
* For the IRQ/BRK vector, an edge in C5 or C6 survives only if /NMI is still
  low in C7 (the vector-high cycle). It is then taken after one handler
  instruction; otherwise it is lost.
* When the NMI vector is used, the NMI is consumed. A held-low /NMI does not
  fire again.
* C7 does not poll, but its RDY repeats do. If RDY stretches C7, an NMI can
  start right after the sequence.

**RDY:**

* The halted cycle is repeated with the same address and SYNC.
* If the repeated cycle is the polling cycle, the poll is evaluated again on
  every repeat.
* CLI and SEI already have the new I during a halted C2. PLP loads only N and V
  early; its I comes after the halt.
* A halted page-crossing dummy read is repeated at the corrected address. This
  applies to abs,X, abs,Y, (zp),Y and forward branches, but not to backward
  branches.
* SHA, SHX, SHY and TAS repeat at the value-substituted address, and the
  following write drops `& (H+1)`, as on real chips.

**RESET:**

* **Power-up hold.** While /RES is held from power-up, every cycle reads
  `(data_of_previous_read << 8) | (previous_address_high - 1)`. This is the
  netlist's real behaviour: the C64's power-up RAM pattern produces addresses
  like $FFFF, $FFFE, $48FE.
* **Release timing.** If /RES goes high in cycle R, cycle R+2 is a SYNC at the
  current address. A BRK-like sequence follows, with three stack reads (S drops
  by 3), $FFFC/$FFFD, and I set. D is not cleared.
* **Reset out of JAM.** After a JAM the bus pattern is $FFFF, $FFFE, $FFFE,
  then $FFFF forever, and IRQ/NMI are ignored. /RES moves the bus to PC three
  cycles after it goes low. JAM $B2 (LDX column) then loads X and N/Z from the
  data bus every cycle until the reset sequence starts.

**Where the netlist differs from real silicon** (fastcpu follows the netlist;
build with `-DFASTCPU_SILICON_IMM` for the silicon formulas, which are not
validated here):

| opcode | real NMOS 6502 | visual6502 netlist (as simulated by netsim.c) |
|---|---|---|
| ANC $0B/$2B | A = A & imm; N/Z; C = N | A unchanged; N/Z from A; C = A bit 7 |
| ALR $4B | A = (A & imm) >> 1 | A = A >> 1, C = bit 0 of (A & imm) |
| ARR $6B | ROR of (A & imm), ARR flags, decimal fix-up | the same with A instead of (A & imm) |
| LAS $BB | A = X = S = M & S | A = S & (M \| $11), X = S, S unchanged, N/Z from M & S |
| XAA $8B / LAX # $AB | "magic" constant ($EE/$FF…) | magic = $00: A = A & X & imm / A = X = A & imm |

The formulas were fitted on 190–300 random netlist cases per opcode, and they
hold in every lockstep run (400 random cases per opcode, the IRQ/RDY sweeps
and the random soup). The likely cause is that the switch-level simulation resolves
these bus fights differently from silicon, so they are reported here rather
than "fixed". No C64 program in normal use depends on them; LAX #, XAA and SHx
are known to be unreliable on real hardware anyway.

## Lockstep validation

`lockstep.c` runs `netcpu` and `fastcpu` from power-on on the same simulated
bus: 64 KB RAM, plus a JMP injector that starts each test case at an opcode
fetch. Every cycle it compares the address, R/W, SYNC and written data. At
every opcode decode it also compares A, X, Y, S and P, which catches register
errors early. On a mismatch it prints the last 40 cycles of both CPUs and
recreates both.

`make test` runs 37 jobs in parallel. It takes about 2.5 minutes on 15 cores
and fails on any mismatch.

| set | what | cycles compared | mismatches |
|---|---|---:|---:|
| opcodes | each of the 244 non-JAM opcodes × 400 random cases: random A/X/Y/S/P (D and I included), instruction at random addresses (¼ at $xxFD–$xxFF), operands biased to page crossings and zero-page wrap, pointers in ZP, BCD operands half the time in decimal mode | 3,163,598 | 0 |
| jam | the 12 JAM opcodes × 8 variants (IRQ held, NMI pulse, RDY low, /RES 3–24 cycles to leave the JAM) | 11,520 | 0 |
| irq | 40 fixed sequences (NOP, LDA #/zp/abs/abs,X±cross, (zp),Y±cross, (zp,X), RMW, JMP, JMP (ind), JSR/RTS, RTI with I=0/1, PHA/PLA/PHP, PLP with every I transition, CLI, SEI, CLI+SEI, SEI+CLI, BRK, branches not taken / taken / taken backward / page-crossing forward and backward, decimal ADC, TXS) and every opcode with random operands; IRQ asserted at every offset −6…+14 for 1, 2, 3 cycles or until the vector fetch, NMI at every offset for 1, 2, 40 cycles; NMI swept over a full IRQ sequence and a BRK | 2,860,531 | 0 |
| rdy | each opcode × 4 random cases × RDY low at every offset −2…+9 for 1, 2, 3, 4, 7, 20 cycles, a quarter of them with an IRQ or NMI pulse in the same window | 2,612,967 | 0 |
| alu | ADC and SBC (zp) for all 65,536 A×M pairs × C × D, run as a 6502 loop that stores each result and the pushed P | 19,413,120 | 0 |
| reset | power-on with /RES held 0–12 cycles on random memory × 9 variants: plain; IRQ held; NMI in the sequence or right after it; RDY low during the hold, around the release, on the reset SYNC, in the sequence, or on the first fetch | 7,020 | 0 |
| random | 8 × 3 M cycles of random valid-opcode soup (JAM bytes fetched as NOP), random jumps, randomly varying IRQ, NMI and RDY waveforms (RDY bursts up to 44 cycles, like badlines and sprites) | 24,000,000 | 0 |
| rom | KERNAL 901227-03 + BASIC 901226-01 on flat RAM with I/O stubs (raster counter, PAL IRQ flag, CIA1 timer IRQ every 16,421 cycles, acknowledged by reading $DC0D; no keys). Boots to `READY.`, then lines are typed into the keyboard buffer: `?2+2` → ` 4`, `?A*A` → ` 49`, a FOR loop with SIN → ` 2.6525449  21` | 6,000,000 | 0 |
| **total** | | **58,068,756** | **0** |

Board level (`make boardtest`, about 40 s): two whole C64 boards from the harness
sources, one with the netlist 6510 and one with fastcpu. The fastcpu board is
the harness's own `c64.c` and `port6510.c`, compiled a second time with
`netcpu_*` bound to fastcpu. Both boot the real ROMs, see VIC badline BA stalls
(108,230 stalled read cycles in 4 M cycles), take CIA IRQs, and have
`PRINT 2+2` plus a BASIC program (`10 FORI=1TO30:POKE53280,I:NEXT`, `RUN`)
typed through the keyboard matrix. The result was **12,000,000 board cycles
(611 frames, 541,835 BA-stalled read cycles, 606 IRQs) with 0 mismatches**,
and the two screens are identical. `make boardtest` runs 4 M of them.

### Mismatch classes found and fixed during development

1. **Power-up /RES hold.** The netlist does not hold a fixed address. It reads
   `data<<8 | (prev_hi-1)`, so random memory exposed it immediately.
2. **ANC, ALR, ARR and LAS** behave differently in the netlist (table above).
   Formulas were fitted on 950 netlist samples.
3. **I-flag timing.** Masking IRQ with I has to happen at poll time, not at
   sample time; the register check at decode exposed it. CLI/SEI keep their I
   change in the last step.
4. **RDY re-poll.** If RDY halts the polling cycle, the netlist polls again on
   every repeat. Found as PLP with a halted pull plus an NMI.
5. **CLI/SEI with a halted C2.** The new I is already in effect during the
   halt, so an IRQ is taken right after CLI.
6. **PLP with a halted pull.** Only N/V load early. A first attempt loaded all
   of P and broke IRQ cases.
7. **Halted page-crossing dummy reads** repeat at the corrected address, but
   only for forward crossings.
8. **SHA/SHX/SHY/TAS under RDY.** The repeat uses the value-substituted
   address, and the write drops `& (H+1)`.
9. **NMI in the last cycles of BRK/IRQ.** The first model (always lost) was
   wrong. Mapped with a 147-case netlist table, then refined twice: re-arming
   was wrong for a /NMI held low, and C7 polls only on RDY repeats.
10. **Reset out of JAM.** The bus moves to PC three cycles after /RES goes low,
    and JAM $B2 loads X.
11. **Branch at $FFFF wrapping to $00xx.** The forward/backward test compared
    addresses instead of page deltas. Found by the random soup with RDY.
12. **RDY during the /RES hold.** Hold and recurrence cycles are reads, so RDY
    halts them. The reset SYNC comes at the first cycle ≥ R+2 that follows an
    un-halted cycle (11 netlist traces).

## Known limits

* **Warm reset** (RES asserted while an instruction runs) is approximate. In
  the netlist the interrupted instruction's write-back, the bus address during
  the hold, and sometimes S depend on internal datapath state in ways not
  modelled here. fastcpu suppresses writes from the second cycle after /RES,
  holds at PC when the instruction ends, and starts the reset sequence in cycle
  R+2, as the netlist does. `lockstep warmreset` (2,000 events in `make test`,
  not counted as pass/fail) measures this, with a KERNAL-like reset handler
  that reloads the registers:
  * **98.3 %** fetch the reset vector in the same cycle;
  * **99.3 %** have an identical bus from the vector fetch on;
  * **16 %** are bus-identical through the whole /RES window.

  The power-up reset, which is the only one the harness does, is exact.
* **Two RES pulses** close together (RES asserted again inside the reset
  sequence) are not modelled.
* **Interrupt timing during a warm reset** is only as good as the warm-reset
  model.

## Speed

`make bench` runs fastcpu alone with a flat-memory callback. On an Apple M5
Pro it measures:

* **~345 M cycles/s** (best of 5: 351) on a loop mixing indexed, indirect,
  RMW, stack and JSR/branch instructions;
* **~300 M cycles/s** on random opcode soup, which defeats branch prediction.

The target was ≥ 50 M cycles/s. The fast path is one predictable branch, one
`switch` on `(opcode << 3) | step`, then building the pin mask.

## Integration: `--cpu fast` in the harness

The 6510 port stays in `port6510.c`, exactly as for the netlist. Only the core
call changes. These are the minimal changes, verified by the board-level
lockstep, which compiles the harness's own `port6510.c`/`c64.c` against
fastcpu this way.

1. **Build.** Copy `fastcpu.c` and `fastcpu.h` to `harness/src/`, or add
   `-I../cpu6510` and `../cpu6510/fastcpu.c`. Then add `src/fastcpu.c` to
   `SRCS`. `fastcpu.h` includes `netcpu.h` for the pin bits.
2. **`port6510.h`.** Add the core selector:
   ```c
   #include "fastcpu.h"
   typedef struct {
       netcpu_t cpu;          /* netlist core (unused with --cpu fast) */
       fastcpu_t fast;        /* behavioural core */
       bool use_fast;
       uint8_t ddr, data, ext; bool port_read; uint16_t port_addr;   /* unchanged */
   } m6510_t;
   void m6510_init_fast(m6510_t *p);
   /* register access for traces/stats, independent of the core */
   uint16_t m6510_pc(const m6510_t *p); uint8_t m6510_a(const m6510_t *p); /* ... x y s p */
   ```
3. **`port6510.c`.** The only functional change is one line in `m6510_tick`:
   ```c
   void m6510_init_fast(m6510_t *p) {
       memset(p, 0, sizeof(*p));
       p->use_fast = true;
       fastcpu_init(&p->fast);
       p->ddr = 0; p->data = 0; p->ext = 0x3F;
   }
   void m6510_free(m6510_t *p) { if (!p->use_fast) netcpu_free(&p->cpu); }
   /* in m6510_tick, replace  pins = netcpu_tick(&p->cpu, pins);  by */
   pins = p->use_fast ? fastcpu_tick(&p->fast, pins) : netcpu_tick(&p->cpu, pins);
   uint8_t m6510_a(const m6510_t *p) { return p->use_fast ? fastcpu_a(&p->fast) : netcpu_a(&p->cpu); }
   /* ... same pattern for pc, x, y, s, p */
   ```
4. **`c64.c`/`c64.h`.**
   * Add `c64_init_cpu(c64_t *m, bool fast)`, or let `main` call
     `m6510_free(&m->cpu); m6510_init_fast(&m->cpu);` right after
     `c64_init()`, before the first tick.
   * The trace line currently calls `netcpu_a(&m->cpu.cpu)` and friends; use
     `m6510_a(&m->cpu)` etc.
   * Nothing else in the board changes: RDY = BA, AEC, /IRQ, /NMI, /RES (8
     cycles), the P0..P5 pull-ups, and the data of the previous read all go in
     exactly as now.
5. **`main.c`.**
   * Accept `--cpu fast|netlist`.
   * Under `--stats`, print the netsim statistics only for the netlist.
   * Take the registers from the `m6510_*` accessors.
   * `--cpu-test`/`--cpu-bench` can gain a fastcpu variant by swapping the tick
     call in `cputest.c`.
6. **Save states (`state.c`).** These currently serialise the netsim node
   state. With `use_fast`, save and restore `m->cpu.fast` with `memcpy`
   instead, and skip `netsim_state_*`.
7. **Power-up.** With both cores starting from their init functions and the
   board holding /RES for 8 cycles, the bus traces are identical from cycle 0.
   The board-level lockstep shows this.

Expected effect: the board becomes VIC/CIA/SID-bound instead of CPU-bound. The
netlist board runs at about 0.1 M cycles/s here; the CPU's own share drops to
about 3 ns per cycle.
