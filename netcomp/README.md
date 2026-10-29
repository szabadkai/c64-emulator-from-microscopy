# netcomp: the visual6502 netlist compiled to C, node-exact against netsim

`ncc.py` compiles the transistor netlist of the NMOS 6502
(`harness/gen/netlist6502.h`, converted from the visual6502 data) into C.
The result, `ccpu`, is a drop-in replacement for `netcpu`. It is **identical
to harness netsim node for node after every recalc of every half cycle**,
floating and charged nodes included.

| | |
|---|---|
| Equivalence | 60.4 M cycles (120.8 M half cycles, 224 M recalcs) with all 1725 nodes compared after every recalc: **0 node mismatches, 0 pin mismatches**. Two complete harness C64 boards (netsim CPU vs. compiled CPU), with all nodes compared every cycle: the BASIC script (4 M cycles) and the demo intro (18 M cycles, 916 frames): **0 mismatches**. |
| Speed, core alone | **172–187 k cycles/s**, against 91–102 k for netcpu in the same benchmark (1.7–2.0×). About 200 k with `make pgo`. That is **0.17–0.19× real time**, so **real time is not reached** (see *Speed*). |
| Speed, whole board | 165 k cycles/s with the compiled core against 100 k with netsim (boot + intro). |
| Main finding | **netsim's stable state depends on its evaluation order.** Reorder the nodes inside its passes, keeping everything else, and node values change in 19–27 % of all cycles. In random code the bus eventually diverges too. So node-exact (and in general even bus-exact) equivalence needs netsim's own event order. That order is what limits the speed. |

`lcpu`, a second, experimental output of the compiler, shows what happens
without that order. It evaluates in a static levelized order, and each step
of it is still a legal switch-level group evaluation. It is bus-identical to
netsim on the KERNAL boot and on documented opcodes, but not node-identical.
It diverges on undocumented opcodes, and it is no faster (~150–200 k cycles/s).

Own code only: Python standard library and C, compiled with clang. Nothing
from third parties was downloaded or run. `harness/`, `cpu6510/`, `vic2/`,
`sid/`, `demo/` and `extract/` are only read and compiled from, never
modified.

## Files

| file | what |
|---|---|
| `ncc.py` | the compiler: netlist header → `gen/ccpu_net.{h,inc}` and `gen/lcpu_net.{h,inc}` |
| `tools/netlist.py` | parses the netlist header and rebuilds netsim's data structures exactly (same clean-up, classification and list orders) |
| `tools/phases.py`, `tools/levelize.py`, `tools/lgen.py` | clock-phase analysis, phase components, SCC levelization and code generation for `lcpu` |
| `src/ccpu.h`, `src/ccpu.c` | the exact core: API, runtime, pin wrapper, save states, netsim state import |
| `src/lcpu.h`, `src/lcpu.c` | the levelized core (experimental) |
| `gen/` | generated code and tables (also rebuilt by `make` when the netlist or the compiler changes) |
| `test/nodestep.c` | node-exact lockstep driver. It includes `../cpu6510/lockstep.c` unchanged (all its scenarios) and binds its netcpu calls to wrappers that run netsim and ccpu side by side |
| `test/statetest.c` | save state / restore / netsim-import test |
| `test/cboard.c`, `test/ccpu_shim.c` | two whole harness C64 boards, netlist vs. compiled (the `--cpu compiled` integration path) |
| `test/bench.c` | speed of the cores alone |
| `test/lstep.c`, `exp/ldiverge.c`, `exp/ldebug.c`, `tools/lroots.py` | lcpu vs. netsim: classified node differences, bus divergence, root-cause check |
| `exp/orderexp.c`, `tools/mkordersim.py` | the order-dependence experiment (netsim with permuted passes vs. netsim) |
| `tools/desc.py` | prints a node's transistors (debug aid) |

### Make targets

| target | what |
|---|---|
| `make` | generate, build everything |
| `make test` | the full suite, run in parallel (≈5 min on 15 cores). It fails on any node or pin mismatch. |
| `make bench` | core-alone speed: ccpu, lcpu and netcpu, plus per-cycle statistics |
| `make boardtest` / `make boardspeed` | board-level lockstep (4 M cycles) and whole-board speed on the intro |
| `make order` | the order-dependence experiment on netsim itself |
| `make lcpu-test` / `make lcpu-roots` | the lcpu results quoted below |
| `make pgo` | clang profile-guided build of the bench |

## Method

### The reference, precisely

netsim (`harness/src/netsim.c`) is a switch-level simulator with an event
queue:

* **Group value.** A node's group is the set of nodes connected to it through
  conducting channel transistors (gate high). Its value follows a fixed
  priority:
  1. conducting to VSS → 0;
  2. else conducting to VCC → 1;
  3. else a pad driven low → 0;
  4. else a pull-up → 1;
  5. else stored charge, 1 if any member holds 1.
* **Passes.** A recalc runs passes over a de-duplicated list of queued nodes.
  It processes the nodes in queue order. A node that changes switches the
  transistors it gates at once, so later nodes in the *same* pass already see
  it. It also queues the nodes that may now change for the next pass.
* **Group walks.** A group is collected by walking per-node lists of
  conducting transistors. Each list is ordered most recently switched on
  first.
* **Order-sensitive rules.** Several rules depend on the order in which
  things happen:
  * pruning (a transistor switching on between equal values queues nothing);
  * the "already evaluated as part of a group in this pass" skip;
  * the order of the fan-out lists, which are sorted by transistor and node
    number.

`ccpu` reproduces all of this exactly. It starts from the same
`netsim_create` clean-up: the netlist's 3510 transistors become 3236, and the
same 16 transistors with their gate on VSS are kept as never-conducting
channels. It evaluates the same queue entries in the same order, walks groups
in the same list order, emits the same queue appends in the same order, and
applies the same loop limit. Power-up is `netcpu_init`'s sequence:
* drive the pads;
* `netsim_stabilize` with every node queued in id order;
* 8 cycles with /RES low.

Even this settle is order dependent (see below), so it too has to be
netsim's.

### What ncc.py generates (gen/ccpu_net.inc)

* **Static node data.**
  * Which nodes are "simple": no channel at all, such as a NOR gate output or
    a pad.
  * Netlist pull-ups.
  * Rail transistors permanently on.
  * The channel-transistor table and its initial link order.
* **Per-node switching code** (netsim's `set_node()` for one node and one new
  value). In order, it lists:
  * the pass transistors to switch on or off, with the "ends differ" queue
    condition;
  * the pull-down and pull-up counts to update, each with its queue
    condition. For simple targets this is the new value of the simple-node
    formula; otherwise it is "count crossed zero";
  * the channel ends to queue when the node falls.

  The compiler resolves statically which condition applies to each target.
  It emits this code twice from one generator:
  * as compact fan-out tables (`CC_FO`, 24,900 entries, the default);
  * as straight-line C per node (`-DCCPU_UNROLLED`, 490 KB of code).
* **Names, pad and register node ids, and sizes.**

### The runtime (src/ccpu.c)

The runtime is netsim's algorithm, rewritten so that the work that cannot
change anything costs no branches:

* **No-op entries.** About 670 of the ~1205 queue entries per cycle change
  nothing. Some are nodes already evaluated in this pass as part of a group.
  The others are simple nodes, or nodes with no conducting channel, whose
  formula gives their current value. The scan loop skips both kinds without
  a data-dependent branch. Neither has a side effect in netsim. The group
  stamp of a single-node group can never be read again, because the pass list
  is de-duplicated.
* **Queue appends.** Appends are branch-free: always store, advance only when
  the entry is new.
* **Transistor lists.** netsim's per-node transistor lists are kept as
  doubly-linked lists with a shared sentinel, so link and unlink need no
  branches.
* **Simple-node formula.** The formula is a table lookup.

The state is a plain struct, 42 KB:
* node flags;
* rail counts;
* lists;
* queue and stamps;
* wrapper state.

`memcpy` is a complete save state. `ccpu_import_netsim()` loads a netsim
save state, converting its list order, so a netlist save state can continue
on the compiled core.

### Channel-connected components (statistics)

| | |
|---|---|
| nodes | 1725 ids, 1704 used, 2 rails |
| transistors | 3510 → 3236 after netsim's clean-up: 2276 to VSS (+16 with gate on VSS), 165 to VCC, 779 channel (pass) transistors |
| simple nodes (no channel) | 736 |
| CCCs | 1024. By size: 736 × 1, 185 × 2, 52 × 3, 35 × 4, 5 × 5, 2 × 6. The 8 datapath bit slices (23–25 nodes each: SB/DB/ADL/ADH bus bits, registers X/Y/S/A, PCL/PCH, data latch, ALU inputs and output). One ALU component of 68 nodes (result multiplexer plus the AND/OR/XOR/carry pass networks). |
| per clock phase (lcpu analysis) | Fixed by CLK0 alone: 51 nodes in PHI1, 78 in PHI2. These include the dpc* datapath controls, which are 0 in PHI2. Phase components: 1166 (PHI1), 1120 (PHI2). Largest gate-dependency cycles: 45 and 40 components (ALU/carry, PHI1) and 45 (PHI2). Self-gated components (a node gates a transistor of its own component): the 8 bit slices in PHI1 (idb_n pulls a node of its own slice), the ALU, #alucout. |

### What is compiled and what is switch-level

Everything in `ccpu` is switch-level, because that is what node-exactness
requires:

* **Compiled.** Everything static is resolved by the compiler and baked into
  tables:
  * classification;
  * fan-out structure and order;
  * queue conditions;
  * initial link order.
* **Done at run time.**
  * Which nodes are queued, and in what order.
  * Group membership (dynamic, through conducting transistors).
  * Group values.

  These are computed exactly as netsim does them.

In `lcpu`, everything except the 68-node ALU is straight-line C: single
nodes as boolean expressions, multi-node components as OR-propagation of a
drive-priority mask over their transistor forest. The ALU (several of its
own nodes gate its transistors) is resolved by a small union-find routine
that only commits steps that are legal sequential group evaluations.

## Equivalence results (ccpu vs. harness netsim)

`make test` runs the `cpu6510` lockstep scenarios through `test/nodestep.c`.
Each scenario compares all 1725 node values of netsim and ccpu after **every
recalc**. There are 4 per cycle:
* data pads driven, end of PHI2;
* CLK0 fall;
* RDY/IRQ/NMI/RES applied;
* CLK0 rise.

fastcpu runs along as a bus-level check. `NODESTEP_TICK=1` instead uses the
real `netcpu_tick()` and `ccpu_tick()`, which also checks the per-recalc
replica of netcpu_tick.

| set | what | cycles | node mismatches |
|---|---|---:|---:|
| opcodes | each of the 244 non-JAM opcodes × 400 random cases (random registers incl. D and I, page crossings, zero-page wrap, BCD operands) | 3.16 M | 0 |
| jam | 12 JAMs × 8 variants (IRQ, NMI, RDY, reset out of the JAM) | 11.5 k | 0 |
| irq | 40 fixed sequences and every opcode; IRQ at every offset −6…+14 for 1/2/3 cycles or until acknowledged; NMI at every offset for 1/2/40 cycles; NMI swept over IRQ and BRK sequences | 2.86 M | 0 |
| rdy | every opcode × 4 cases × RDY low at every offset −2…+9 for 1–20 cycles, some with IRQ/NMI | 2.61 M | 0 |
| alu | ADC and SBC for all 65,536 A×M pairs × C × D | 19.4 M | 0 |
| reset | power-on with /RES held 0–12 cycles × 9 variants | 7 k | 0 |
| warm reset | /RES asserted while running, 2000 events (approximate in fastcpu, exact here) | 0.96 M | 0 |
| random | 8 × 3 M cycles of random-opcode soup with random IRQ/NMI/RDY waveforms | 24 M | 0 |
| rom | KERNAL + BASIC boot on flat RAM with I/O stubs, BASIC lines typed (`?2+2` → ` 4`, a FOR/SIN loop → ` 2.6525449  21`) | 6 M | 0 |
| tick mode | random 1 M + IRQ sequences, using the real netcpu_tick/ccpu_tick | 1.4 M | 0 |
| **total** | **120.8 M half cycles, 224 M recalcs, 3.87 × 10¹¹ node comparisons** | **60.4 M** | **0** |

* **Bus level.** The same runs compared ccpu and netsim pins every cycle, and
  fastcpu against netsim on 59.5 M cycles. The result was 0 mismatches.
* **Power-up.** `ccpu_init()` equals `netcpu_init()` node for node. This is
  checked at every power-up in the suite (about 250, including 117 reset
  variants and 96 JAM cases).
* **Save states.** `statetest`: save mid-run, load into another core, and the
  continuation is identical (pins every cycle, all nodes at the end). A
  struct copy works the same way. A netsim state imported after 50 k cycles
  of netcpu continues node for node with netcpu.
* **Board level (`cboard`).** Two whole C64 boards built from the harness
  sources, with real VIC badlines and BA/AEC, CIAs, PLA, SID and keyboard.
  Board A uses netsim; board B uses ccpu through the harness's own `c64.c`
  and `port6510.c`, compiled a second time. CPU pins and all 1725 nodes are
  compared after every cycle:
  * BASIC script (`PRINT 2+2`, a POKE loop): 4 M cycles, 204 frames, 108,230
    BA-stalled reads, 6.9 × 10⁹ node comparisons, **0 mismatches**;
  * demo intro (`--prg minimalistic_fig.prg`, RUN at READY.): 18 M cycles,
    916 frames, 1.74 M BA-stalled reads, 583 IRQs, 236 NMIs, 3.1 × 10¹⁰ node
    comparisons, **0 mismatches**, screen and RAM identical.

## Order dependence of netsim

This is the central finding. netsim's result is not determined by the
netlist and the inputs alone. It also depends on the order in which its
queue processes nodes inside a pass.

**Experiment (`make order`).** `tools/mkordersim.py` makes a copy of the
current `netsim.c` in which only the processing order of each pass is
changed (shuffled, or reversed). The group rule, pruning and passes are
netsim's. Two netcpus run on the same bus from the same state. All nodes are
compared after every cycle; on a difference the copy is resynchronised, so
every count is a fresh effect.

| order | workload | cycles | cycles with node differences | distinct nodes | bus differences |
|---|---|---:|---:|---:|---:|
| shuffled passes | KERNAL boot | 400 k | 19.0 % | 31 (all unnamed) | 0 |
| reversed passes | KERNAL boot | 400 k | 22.7 % | 20 (all unnamed) | 0 |
| shuffled passes | random soup + IRQ/NMI/RDY | 200 k | 21.5 % | 158 (44 named) | 0 |
| reversed passes | random soup + IRQ/NMI/RDY | 200 k | 27.2 % | 54 | 0 |
| shuffled, no resync | random soup | 200 k | 62.7 % | 451 | 3 (first at cycle 143,740) |

The power-up settle (`netsim_stabilize` from all-zero nodes) is order
dependent as well: 32 and 110 nodes differ after `netcpu_init` under the two
permutations.

**Mechanism.** Within a pass, netsim evaluates nodes in queue order, and a
change is visible to every later node of the same pass. Whether a node sees
an input's new value or its old one therefore depends on queue positions,
which follow from the history of the pass (breadth-first discovery order).

A throwaway instrumented netsim, not kept, measured this over 100 k KERNAL
cycles:
* about 38 times per cycle, a node is evaluated in the same pass in which
  one of its gate inputs changes;
* of the 402 such (input, node) pairs, 75 occur in both orders, about
  1.9 times per cycle.

No static order reproduces this.

**What is affected.** The nodes that pick up the differences are floating
nodes: their group has no path to VSS or VCC, no pad and no pull-up, so they
keep charge. For example:
* the internal nodes of series pull-down stacks in the ALU carry/XOR logic
  (n1013, n136, n547 …);
* the data-latch junction n1424 between the DL/DB, DL/ADL and DL/ADH pass
  gates;
* the dynamic ALU input latches alua*/alub*.

A typical race: at the PHI2 edge, does idb2 change before the DL/DB gate
closes? Under netsim's order, idb2 changes first, so n1424 captures a 1.
Under the levelized order, the gate closes first, so n1424 keeps its 0. The
delay-free switch-level model has no notion of which comes first. netsim's
breadth-first order acts as an implicit unit-delay timing model.

**Consequence.**
* **Node-exact equivalence.** Any engine with a different evaluation order
  differs from netsim on these nodes from the first half cycle on.
* **Bus equivalence.** In general bus-exact equivalence fails too, because
  some floating charges feed dynamic logic later. In practice this happened
  only through unstable undocumented opcodes (see lcpu below).

ccpu therefore replays netsim's event order exactly. That is why its node
mismatch count is 0, and why its speed is bounded by that order.

## Speed

`make bench` runs the core alone: flat 64 KB, KERNAL boot with I/O stubs and
a mixed-addressing loop. Apple M5 Pro, single thread, best of 3.

| | KERNAL boot | mixed loop |
|---|---:|---:|
| harness netcpu (netsim) | 102 k cycles/s | 91–102 k cycles/s |
| **ccpu** (default, table-driven) | **172–179 k** | **180–187 k** |
| ccpu, `make pgo` | 189–191 k | 199–200 k |
| ccpu `-DCCPU_UNROLLED` (straight-line C per node) | 140 k | 146 k |
| lcpu (levelized, *not* node-exact) | 150–206 k | 190–230 k |

These are **0.17–0.20× real time** (985,248 cycles/s). Whole-board speed is
165 k cycles/s with ccpu against 100 k with netsim (`make boardspeed`,
intro). The fast behavioural core (cpu6510) remains the real-time option.

### Evaluations per cycle, before and after

Exactness fixes the schedule. ccpu processes exactly netsim's 36.2 passes
and 1205 queue entries (1184 group evaluations) per cycle, with 449 node
changes (KERNAL boot). What changed is the cost of each entry:

| per cycle | netsim | ccpu |
|---|---:|---:|
| passes | 36.2 | 36.2 |
| queue entries / group evaluations | 1205 / 1184 | 1205, of which ~670 are skipped branch-free |
| real work | group walk for every non-simple entry | 282 lone-node changes + 253 group walks (585 nodes) |
| node changes | 449 | 449 |
| fan-out operations | 694 transistor switches, 194 + 94 rail-count updates, 681 queue appends | same, branch-free |
| CPU cycles per 6502 cycle (≈4.5 GHz) | ≈44 k | ≈25 k |

### What limits it (profile)

`sample` on the table-driven core:

| share | where |
|---:|---|
| 47 % | switching code (`set_node`, fan-out loops) |
| 30 % | the pass scan |
| 23 % | group walks |

Real time needs ≤ ~4.5 k CPU cycles per 6502 cycle. That leaves about 4
cycles per queue entry, or 10 per node change.

The work is a long, data-dependent sequence: which node comes next, and
whether it changes. Each actual change costs about one to two branch
mispredictions (≈15 cycles each) on top of the table walk. ~450 changes and
~250 group walks per cycle alone account for ~15–20 k cycles.

Things tried and measured:

| approach | result | why |
|---|---|---|
| straight-line per-node C | 140 k (slower) | 490 KB text against a 192 KB L1I; the I-cache misses outweigh the per-node branch prediction |
| straight-line code for the 100/200/400 hottest nodes only | 141–157 k (slower) | the change set is spread out: the top 200 nodes make only 48 % of the changes |
| switch-on timestamps instead of netsim's linked lists | 132 k (slower) | group walks then scan all channels of bus nodes and sort |
| PGO | +7 % | |
| a static levelized schedule (lcpu, below) | not node-exact, and not faster | an oblivious sweep touches all 2286 phase components per cycle, plus ~13 SCC iterations and ~13 ALU rounds |

A real-time core would have to:
* evaluate each half cycle in roughly one pass of straight-line, mostly
  branch-free code;
* stay inside the I-cache.

That is only possible with a *static* schedule, and the order dependence
above rules out a static schedule that equals netsim. So real time and
node-exact netsim equivalence are, as far as this work shows, mutually
exclusive on one core. The half cycle is far too short (~0.5 µs) for
cross-core synchronisation.

## lcpu: the levelized core (experiment)

`lcpu` uses the same netlist and the same group rule, with a static order:

1. **Clock analysis.** For each clock phase, ternary (0/1/X) propagation
   with only CLK0 known finds the nodes fixed by the clock. This is
   mechanical, from the netlist, with pads treated as external.
2. **Phase components.** Transistors fixed off are dropped and those fixed
   on are merged. What remains splits into phase components.
3. **Topological order.** The gate-dependency graph of the components is
   ordered topologically, with Tarjan SCCs iterated to a fixed point.
4. **Code generation.** One straight-line C function per phase, plus the
   cones of the data pads and of RDY/IRQ/NMI/RES.

Every step is one of netsim's group evaluations on the state at that moment.
A legal step order is guaranteed for the self-gated components:
* a component with one self-gating node is evaluated simultaneously, then
  repeated;
* the ALU commits only groups that cannot influence each other.

So lcpu computes a result netsim could produce under some other queue order.

Results (`make lcpu-test`, `make lcpu-roots`):

* **Node level vs. netsim.** Differences start in the first half cycles.
  Over 3000 recalcs with differences, starting each from netsim's state,
  every *root* difference was a floating node (3289 roots, 0 driven). A root
  is a node whose inputs and component all agree. All driven-node
  differences are downstream of floating ones. The evaluator is a correct
  switch-level evaluator; the differences are order effects.
* **Bus level, no resync.**
  * Identical to netsim on the KERNAL boot (3 M cycles, reaches READY and
    prints ` 4`).
  * Identical on documented-opcode random soup (20 × 300 k cycles).
  * Random soup with undocumented opcodes diverges within hundreds to
    thousands of cycles, for example a different byte written by a later
    STA/PHA.

  These are the unstable opcodes whose netsim behaviour depends on bus
  fights and charge (cf. `cpu6510/README.md`, "Where the netlist differs
  from real silicon").
* **Speed.** ~150–230 k cycles/s, no better than ccpu, for the reasons
  above.

lcpu is therefore not offered as an equivalent core. It documents what
dropping netsim's order costs (node identity, unstable opcodes) and gains
(nothing in speed).

## Integration: `--cpu compiled` in the harness

The pattern is the one `cpu6510/README.md` uses for `--cpu fast`. The 6510
port stays in `port6510.c`; only the core call changes. `test/cboard.c`
already builds the harness's own `c64.c`/`port6510.c` against ccpu this way
(via `test/ccpu_shim.c`) and runs it in lockstep with the netlist board.

1. **Build.** Generate once (`python3 -I ../netcomp/ncc.py
   gen/netlist6502.h ../netcomp/gen`, or `make -C ../netcomp`). Add
   `-I../netcomp/src` and `../netcomp/src/ccpu.c` to `SRCS`, like
   `ext_fastcpu.o`. `ccpu.h` includes `netcpu.h` for the pin bits.
2. **`port6510.h`.** Add `#include "ccpu.h"`, a core selector in place of
   `bool use_fast`, and an init:
   ```c
   enum { CORE_NETLIST, CORE_FAST, CORE_COMPILED } core;
   ccpu_t comp;
   void m6510_init_compiled(m6510_t *p);
   ```
3. **`port6510.c`.** In `m6510_tick`:
   ```c
   pins = p->core == CORE_COMPILED ? ccpu_tick(&p->comp, pins)
        : p->core == CORE_FAST     ? fastcpu_tick(&p->fast, pins) : netcpu_tick(&p->cpu, pins);
   ```
   `m6510_init_compiled`: `memset(p, 0, sizeof *p); ccpu_init(&p->comp);
   p->core = CORE_COMPILED; p->ext = 0x3F;`. The accessors
   `m6510_pc/a/x/y/s/p` use `ccpu_pc(&p->comp)` and so on. `m6510_free`:
   nothing to free.
4. **`c64.c` / `main.c`.**
   * Add `c64_use_compiled_cpu(m)`, analogous to `c64_use_fast_cpu`.
   * Accept `--cpu compiled`.
   * `--stats` can print `ccpu_stats()`: passes, queue entries, changes.
   * Everything else on the board stays as it is (RDY = BA, AEC, /IRQ, /NMI,
     8 cycles of /RES, P0–P5).

   Bus traces are identical to the netlist board from cycle 0.
5. **Save states (`state.c`).** Save `ccpu_state_save()` (42 KB, magic +
   size checked) instead of `netsim_state_save`. To resume a *netlist* save
   state on the compiled core, call `ccpu_import_netsim(&p->comp, buf, len,
   prev_read, db_driven)`. Node values, pulls, counts and list order carry
   over exactly. The other direction is not needed for ccpu, since it
   produces identical states.
6. **Expected effect.** CPU-bound board time drops by about 40 %: 165 k
   instead of 100 k cycles/s on the intro. For real time use `--cpu fast`.
   `--cpu compiled` is for runs that must be the netlist, node for node, at
   1.7–1.8× the speed.

## Honest limits

* **Real time is not reached.** The exact core runs at 0.17–0.20× real time
  alone and 0.17× on the full board. The analysis above is why I believe a
  netsim-exact core cannot be fundamentally faster on one core.
* **Exactness rests on reproducing netsim's algorithm.** That is verified on
  60 M cycles plus 22 M board cycles. A future change to `netsim.c` (its
  queue order, pruning or list order) must be mirrored in `ccpu.c` and
  `tools/netlist.py`. `make test` would show it immediately.
* **netsim's `LOOP_LIMIT` (1000 passes) is replicated.** It was never hit in
  any test.
* **The order-dependence numbers are scoped.** They come from netsim with
  permuted passes, and the within-pass pair statistics from a throwaway
  instrumented copy. Both show that differences exist and how often; they do
  not characterise every possible order.
* **lcpu is an experiment.** It is not node-exact, it is not bus-exact on
  unstable undocumented opcodes, and it is not faster.
