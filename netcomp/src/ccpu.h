/* ccpu.h - the visual6502 NMOS 6502 netlist compiled to C by ncc.py.
 *
 * Same pin interface and semantics as harness/src/netcpu.h:
 *
 *     pins = ccpu_tick(&cpu, pins);      one call == one PHI2 cycle
 *
 * Inputs: D0..D7 = data of the PREVIOUS cycle's read (ignored after a
 * write), IRQ/NMI/RES (set = asserted, line low) and RDY (set = high) for
 * the new cycle.  Outputs: A0..A15, RW (set = read), SYNC, and D0..D7 when
 * RW = 0.  All other bits pass through.  The CPU_* bit definitions are
 * those of netcpu.h.
 *
 * ccpu_init() performs exactly the power-up of netcpu_init() (drive the pads,
 * settle every node, 8 clock cycles with /RES low) and ends in the same
 * node state, node for node.
 *
 * ccpu_t is plain data (no pointers): memcpy() is a complete save state,
 * ccpu_state_save()/ccpu_state_load() wrap that with a version check.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netcpu.h"            /* CPU_* pin bits, CPU_GET/SET_* helpers */
#include "../gen/ccpu_net.h"   /* CC_NN, CC_NSLOT: sizes of the compiled netlist */

typedef struct {
    uint64_t passes;          /* netsim passes */
    uint64_t evals;           /* queue entries processed (= netsim's group evaluations + skips) */
    uint64_t lone;            /* entries that changed a node with no conducting channel */
    uint64_t walks;           /* group walks (node with conducting channels), and their nodes */
    uint64_t group_nodes;
    uint64_t changes;         /* node value changes */
    uint64_t loop_limit_hits;
} ccpu_stats_t;

typedef struct {
    /* node state (same encoding as netsim: bit0 value, bit1 pull-up, bit2 pull-down) */
    uint8_t f[CC_NN];
    uint8_t ngnd[CC_NN], npwr[CC_NN];  /* conducting transistors to VSS / VCC */
    /* conducting channel transistors per node, in netsim's list order:
       doubly linked through nxt/prv; index 0 = end sentinel, 1..CC_NSLOT =
       transistor ends, CC_NSLOT+1+n = list head of node n */
    uint16_t nxt[1 + CC_NSLOT + CC_NN], prv[1 + CC_NSLOT + CC_NN];
    /* evaluation queue */
    uint16_t q[2][CC_NN + 1];
    int qo, nout;                      /* q[qo] is the list being filled */
    uint32_t omark[CC_NN], gmark[CC_NN];
    uint32_t ostamp, gstamp;
    uint16_t group[CC_NN];
    /* pin wrapper (as netcpu_t) */
    bool prev_read;
    int db_driven;
    uint64_t cycles;
    ccpu_stats_t st;
} ccpu_t;

void     ccpu_init(ccpu_t *c);                 /* power-up, leaves /RES asserted */
void     ccpu_free(ccpu_t *c);                 /* nothing to free; for symmetry */
uint64_t ccpu_tick(ccpu_t *c, uint64_t pins);

/* low level (same meaning as netsim_drive/release/recalc) */
void ccpu_drive(ccpu_t *c, int node, int level);
void ccpu_release(ccpu_t *c, int node);
void ccpu_recalc(ccpu_t *c);

/* introspection */
int      ccpu_node_id(const char *name);       /* -1 if unknown */
const char *ccpu_node_name(int node);          /* first visual6502 name, NULL if unnamed */
int      ccpu_group_drive(const ccpu_t *c, int node);  /* 0 floating, 1 pull-up, 2 pad, 3 VSS/VCC */
static inline int ccpu_node(const ccpu_t *c, int node) { return c->f[node] & 1; }
uint8_t  ccpu_a(const ccpu_t *c);
uint8_t  ccpu_x(const ccpu_t *c);
uint8_t  ccpu_y(const ccpu_t *c);
uint8_t  ccpu_s(const ccpu_t *c);
uint8_t  ccpu_p(const ccpu_t *c);              /* bit5 = 1, bit4 = 0 like netcpu_p */
uint16_t ccpu_pc(const ccpu_t *c);
uint8_t  ccpu_ir(const ccpu_t *c);
const ccpu_stats_t *ccpu_stats(const ccpu_t *c);

/* import a harness netsim_state_save() buffer (+ netcpu_t prev_read/db_driven) */
int ccpu_import_netsim(ccpu_t *c, const void *buf, size_t len, int prev_read, int db_driven);

/* save states */
size_t ccpu_state_size(void);
void   ccpu_state_save(const ccpu_t *c, void *buf);
int    ccpu_state_load(ccpu_t *c, const void *buf, size_t len);   /* 0 = ok */
