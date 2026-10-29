/* lcpu.h - the visual6502 netlist compiled to levelized straight-line C
 * (ncc.py / tools/lgen.py): the fast, real-time variant of ccpu.
 *
 * Same pin interface and semantics as netcpu_tick()/ccpu_tick() (CPU_* bits
 * of harness/src/netcpu.h).  Every recalc is a legal switch-level (netsim)
 * evaluation -- each step evaluates a connected group of nodes from the
 * state at that moment -- but in a static, levelized order instead of
 * netsim's event queue order.  netsim's result depends on that order (see
 * README.md, "Order dependence"), so lcpu matches netsim on every driven
 * node and on the bus, while charge left on floating nodes by evaluation
 * order artefacts (glitches) can differ.  Use ccpu for node-exact netsim
 * equivalence.
 *
 * lcpu_t is plain data: memcpy() is a complete save state.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netcpu.h"
#include "../gen/ccpu_net.h"
#include "../gen/lcpu_net.h"

typedef struct {
    uint64_t sweeps, cones, scc_iters, sg_rounds, loop_limit_hits;
} lcpu_stats_t;

typedef struct {
    uint8_t v[CC_NN];            /* node values */
    uint8_t pad[LC_NPADS];       /* pad drive: 0 none, 4 driven high, 8 driven low */
    bool prev_read;
    int db_driven;
    uint64_t cycles;
    lcpu_stats_t st;
} lcpu_t;

void     lcpu_init(lcpu_t *c);           /* = netcpu_init's power-up state, node for node */
void     lcpu_free(lcpu_t *c);
uint64_t lcpu_tick(lcpu_t *c, uint64_t pins);

/* the four recalcs of a tick, for tests that compare after each one */
void lcpu_step_db(lcpu_t *c, int d);          /* drive D0..D7 (end of PHI2) + recalc */
void lcpu_step_phi1(lcpu_t *c);               /* CLK0 low + recalc */
void lcpu_step_ctl(lcpu_t *c, uint64_t pins); /* RDY/IRQ/NMI/RES + recalc */
void lcpu_step_phi2(lcpu_t *c);               /* CLK0 high + recalc */
uint64_t lcpu_outputs(lcpu_t *c, uint64_t pins);

static inline int lcpu_node(const lcpu_t *c, int node) { return c->v[node]; }
uint8_t  lcpu_a(const lcpu_t *c);
uint8_t  lcpu_x(const lcpu_t *c);
uint8_t  lcpu_y(const lcpu_t *c);
uint8_t  lcpu_s(const lcpu_t *c);
uint8_t  lcpu_p(const lcpu_t *c);
uint16_t lcpu_pc(const lcpu_t *c);
uint8_t  lcpu_ir(const lcpu_t *c);
const lcpu_stats_t *lcpu_stats(const lcpu_t *c);

/* load node values and pad drives (e.g. from a netsim/ccpu state) */
void lcpu_set_nodes(lcpu_t *c, const uint8_t *values, const uint8_t *pad_flags_netsim);

size_t lcpu_state_size(void);
void   lcpu_state_save(const lcpu_t *c, void *buf);
int    lcpu_state_load(lcpu_t *c, const void *buf, size_t len);
