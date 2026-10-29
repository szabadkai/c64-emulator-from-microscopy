/* fastcpu.h - fast behavioural NMOS 6502 core, bus-cycle-identical to the
 * visual6502 netlist CPU (netcpu.c), with the same pin interface.
 *
 *     pins = fastcpu_tick(&cpu, pins);     one call == one PHI2 cycle
 *
 * Pin bits are the CPU_* definitions of netcpu.h, with the same meaning:
 *   in:  D0..D7  data of the PREVIOUS cycle's read (ignored after a write)
 *        IRQ, NMI, RES   set = line asserted (low) during THIS cycle
 *        RDY             set = RDY high; low halts THIS cycle if it is a read
 *   out: A0..A15, RW (set = read), SYNC (opcode fetch), D0..D7 on writes.
 *        All other bits (and D0..D7 on reads) are passed through unchanged.
 * The 6510 I/O port ($00/$01, P0..P5, AEC) is not part of the core: wrap it
 * exactly like port6510.c wraps netcpu (see README.md).
 *
 * fastcpu_init() leaves the core in the same state as netcpu_init(): /RES
 * held, address bus at $00FF, registers at the values the netlist settles
 * to at power-up (PC=$00FF A=$FF X=$44 Y=$00 S=$C4 P=$A0).  While /RES is
 * held the bus reads (data << 8) | (previous high byte - 1) like the
 * netlist; if /RES goes high in cycle R, cycle R+2 is the SYNC that starts
 * the reset sequence.  Power-up reset is exact; /RES asserted while running
 * ("warm reset") is approximate during the /RES window (README.md).
 *
 * fastcpu_t is plain data: copy it to save or restore the CPU state.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "netcpu.h"   /* CPU_* pin bits and CPU_GET/SET_* helpers */

typedef struct {
    uint16_t pc;
    uint8_t a, x, y, s, p;
} fastcpu_regs_t;

typedef struct {
    /* programmer-visible registers */
    uint16_t pc;
    uint8_t a, x, y, s, p;
    /* sequencer */
    uint8_t ir;        /* opcode being executed (0 for IRQ/NMI/RESET too) */
    uint8_t t;         /* step within the instruction */
    uint8_t brk;       /* for ir==0: 0 = BRK opcode, 1 = IRQ/NMI, 2 = RESET */
    uint8_t sync;      /* the cycle just output was an opcode fetch */
    uint16_t ad;       /* address latch */
    uint8_t dl;        /* data latch */
    uint8_t hb;        /* base address high byte (SHA/SHX/SHY/TAS) */
    uint8_t fix;       /* this cycle is a page-crossing dummy read; ad = fixed address */
    uint8_t sh_halt;   /* RDY halted the dummy read of SHA/SHX/SHY/TAS */
    /* interrupts */
    uint8_t intr;      /* poll result: next opcode fetch becomes an interrupt */
    uint8_t pcyc;      /* the cycle just put out is the instruction's polling cycle */
    uint8_t irq_s;     /* /IRQ was asserted in the last cycle */
    uint8_t nmi_f;     /* NMI edge seen, not yet serviced */
    uint8_t nmi_prev;  /* /NMI asserted in the previous cycle */
    uint8_t nmi_chk;   /* IRQ/BRK vector-high cycle: drop nmi_f unless /NMI is low */
    /* RDY, RES, JAM */
    uint8_t halted;    /* last cycle was a read stopped by RDY: repeat it */
    uint8_t res;       /* 0 = run, 1 = power-up /RES hold, 2 = warm-reset / JAM hold */
    uint8_t rel;       /* cycles since /RES was released (hold states) */
    uint8_t res_age;   /* warm reset: cycles since /RES was first seen (saturates) */
    uint8_t res_pend;  /* next opcode fetch starts the reset sequence */
    uint8_t jam;       /* executed a JAM/KIL opcode */
    uint32_t hold;     /* A0..A15 | RW | SYNC of the last cycle (for RDY repeats) */
    uint64_t cycles;   /* ticks since init */
} fastcpu_t;

void     fastcpu_init(fastcpu_t *c);             /* power-up, /RES held */
uint64_t fastcpu_tick(fastcpu_t *c, uint64_t pins);

/* registers (P as netcpu_p reports it: bit 5 = 1, bit 4 = 0) */
fastcpu_regs_t fastcpu_regs(const fastcpu_t *c);
void     fastcpu_set_regs(fastcpu_t *c, const fastcpu_regs_t *r);  /* e.g. power-up values */
uint16_t fastcpu_pc(const fastcpu_t *c);
uint8_t  fastcpu_a(const fastcpu_t *c);
uint8_t  fastcpu_x(const fastcpu_t *c);
uint8_t  fastcpu_y(const fastcpu_t *c);
uint8_t  fastcpu_s(const fastcpu_t *c);
uint8_t  fastcpu_p(const fastcpu_t *c);
uint8_t  fastcpu_ir(const fastcpu_t *c);
bool     fastcpu_jammed(const fastcpu_t *c);
