/* netsim.h - generic switch-level NMOS netlist simulator.
 *
 * Model: every node is an electrical net.  A node may carry a pull-up
 * (depletion load) from the netlist.  Transistors connect c1<->c2 while their
 * gate node is high.  After any input change, the connected group of every
 * affected node is recomputed:
 *     group touches VSS          -> 0
 *     else group touches VCC     -> 1
 *     else externally pulled low -> 0
 *     else has a pull-up         -> 1 (pull-ups and external highs)
 *     else floating: 1 if any member still holds charge, else 0
 * Nodes whose value changed switch the transistors they gate; the nodes on
 * the far side of those transistors are queued for the next pass.  Passes
 * repeat until nothing changes (or a loop limit is hit).
 *
 * The engine knows nothing about the 6502; netcpu.c binds it to the
 * visual6502 data.  A future netlist-derived chip (CIA, VIC, ...) can reuse
 * it unchanged. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct netsim netsim_t;

typedef struct {
    uint64_t passes;        /* recalc passes */
    uint64_t groups;        /* group evaluations */
    uint64_t group_nodes;   /* sum of group sizes */
    uint64_t loop_limit_hits;
} netsim_stats_t;

/* trans: ntrans rows of {gate, c1, c2}; pullups: list of pulled-up nodes */
netsim_t *netsim_create(int nnodes, int ntrans, const uint16_t (*trans)[3],
                        const uint16_t *pullups, int npullups, int vss, int vcc);
void netsim_destroy(netsim_t *s);

/* drive a node from outside (pad input).  Queues it; call netsim_recalc(). */
void netsim_drive(netsim_t *s, int node, int level);
/* stop driving a node from outside (back to its netlist pull-up / float) */
void netsim_release(netsim_t *s, int node);
/* evaluate until stable */
void netsim_recalc(netsim_t *s);
/* queue every node and evaluate (power-up) */
void netsim_stabilize(netsim_t *s);
int  netsim_get(const netsim_t *s, int node);
int  netsim_num_transistors(const netsim_t *s);
const netsim_stats_t *netsim_stats(const netsim_t *s);

/* savestate of the complete dynamic state (node values, pulls, rail counts,
   conducting transistors incl. the order of the live adjacency lists, stamps,
   statistics).  Load requires an instance created from the same netlist. */
size_t netsim_state_size(const netsim_t *s);
void   netsim_state_save(const netsim_t *s, void *buf);
int    netsim_state_load(netsim_t *s, const void *buf, size_t len);   /* 0 = ok */
