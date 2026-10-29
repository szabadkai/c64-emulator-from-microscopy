/* netcpu.h - NMOS 6502 driven by the visual6502 transistor netlist.
 *
 * Pin interface (one call == one full PHI2 cycle):
 *
 *     pins = netcpu_tick(&cpu, pins);
 *
 * On entry, pins carry the inputs for the cycle that is about to start:
 *   - D0..D7: the data the board fetched for the PREVIOUS cycle's read
 *             (ignored if the previous cycle was a write).  It is applied
 *             while CLK0 is still high, i.e. at the end of the previous PHI2,
 *             just as a real bus would hold it until the falling edge.
 *   - IRQ, NMI, RES (bit set = line asserted/low), RDY (bit set = line high)
 *             for the new cycle; applied right after CLK0 falls, i.e. during
 *             its PHI1 (RDY low then halts the CPU in this very cycle if it
 *             is a read; writes ignore RDY, as on the NMOS 6502).
 * The call then runs PHI1 (CLK0 low) and PHI2 (CLK0 high) of the new cycle and
 * returns the outputs: A0..A15, RW (set = read), SYNC, and D0..D7 when RW=0.
 * The board performs the access and, for a read, places the byte on D0..D7
 * for the next call.
 *
 * The same bit layout is used by the 6510 wrapper (port6510.h), which adds
 * AEC and the P0..P5 I/O port pins. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "netsim.h"

#define CPU_A_MASK   0xFFFFULL
#define CPU_D_SHIFT  16
#define CPU_D_MASK   (0xFFULL << CPU_D_SHIFT)
#define CPU_RW       (1ULL << 24)   /* out: 1 = read, 0 = write */
#define CPU_SYNC     (1ULL << 25)   /* out: opcode fetch */
#define CPU_IRQ      (1ULL << 26)   /* in:  1 = /IRQ asserted (line low) */
#define CPU_NMI      (1ULL << 27)   /* in:  1 = /NMI asserted (line low) */
#define CPU_RES      (1ULL << 28)   /* in:  1 = /RES asserted (line low) */
#define CPU_RDY      (1ULL << 29)   /* in:  1 = RDY high (CPU may run) */
#define CPU_AEC      (1ULL << 30)   /* in (6510): 1 = AEC high, CPU owns PHI2 bus */
#define CPU_P_SHIFT  32             /* 6510 port P0..P5: bits 32..37 */
#define CPU_P_MASK   (0x3FULL << CPU_P_SHIFT)

#define CPU_GET_ADDR(p)    ((uint16_t)((p) & CPU_A_MASK))
#define CPU_GET_DATA(p)    ((uint8_t)(((p) >> CPU_D_SHIFT) & 0xFF))
#define CPU_SET_DATA(p, d) (((p) & ~CPU_D_MASK) | ((uint64_t)(uint8_t)(d) << CPU_D_SHIFT))

typedef struct {
    netsim_t *sim;
    int n_ab[16], n_db[8];
    int n_rw, n_sync, n_clk0, n_rdy, n_irq, n_nmi, n_res, n_so;
    int n_a[8], n_x[8], n_y[8], n_s[8], n_p[8], n_pcl[8], n_pch[8], n_ir[8];
    bool prev_read;     /* previous cycle was a read: drive D0..D7 on entry */
    int db_driven;      /* last byte driven onto the data pads, -1 = none */
    uint64_t cycles;
} netcpu_t;

void     netcpu_init(netcpu_t *c);          /* power-up, leaves /RES asserted */
void     netcpu_free(netcpu_t *c);
uint64_t netcpu_tick(netcpu_t *c, uint64_t pins);

/* introspection (reads storage nodes of the netlist) */
int      netcpu_node_id(const char *name);  /* -1 if unknown */
int      netcpu_node(const netcpu_t *c, int node);
uint8_t  netcpu_a(const netcpu_t *c);
uint8_t  netcpu_x(const netcpu_t *c);
uint8_t  netcpu_y(const netcpu_t *c);
uint8_t  netcpu_s(const netcpu_t *c);
uint8_t  netcpu_p(const netcpu_t *c);
uint16_t netcpu_pc(const netcpu_t *c);
uint8_t  netcpu_ir(const netcpu_t *c);
int      netcpu_transistors(const netcpu_t *c);
