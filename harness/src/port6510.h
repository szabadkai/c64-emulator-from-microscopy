/* port6510.h - MOS 6510 = netlist NMOS 6502 core + hand-written I/O port.
 *
 * Pins: the CPU_* layout of netcpu.h plus
 *   CPU_AEC          in:  AEC high -> CPU drives the bus in PHI2.  When low the
 *                         board must ignore the CPU's address/RW (tri-stated).
 *   P0..P5 (32..37)  in:  level the outside world pulls each port pin to when
 *                         the 6510 is not driving it (C64: pull-ups, cassette
 *                         sense switch on P4);
 *                    out: actual pin level (driven value for outputs).
 * $0000 = DDR (1 = output), $0001 = data.  Reads of $00/$01 are answered by
 * the port (the external bus value is replaced); writes are latched by the
 * port AND still appear on the external bus, so RAM under $00/$01 is written
 * as on a real C64. */
#pragma once
#include "netcpu.h"
#include "fastcpu.h"   /* ../cpu6510: cycle-exact behavioural core, lockstep-verified */
#include "ccpu.h"      /* ../netcomp: the netlist compiled to C, node-exact against netsim */

enum { M6510_NETLIST, M6510_FAST, M6510_COMPILED };

typedef struct {
    netcpu_t cpu;         /* transistor-level core (default) */
    fastcpu_t fast;       /* behavioural core */
    ccpu_t comp;          /* compiled netlist core */
    int core;             /* M6510_NETLIST (default), M6510_FAST, M6510_COMPILED */
    uint8_t ddr, data;
    uint8_t ext;          /* external levels of P0..P5 (last tick) */
    bool port_read;       /* previous cycle was a read of $00/$01 */
    uint16_t port_addr;
} m6510_t;

void     m6510_init(m6510_t *p);        /* netlist core */
void     m6510_init_fast(m6510_t *p);   /* fast core: bus-cycle identical, ~350x real time */
void     m6510_init_compiled(m6510_t *p);   /* compiled netlist: node-identical, ~1.75x netlist speed */
void     m6510_free(m6510_t *p);
uint64_t m6510_tick(m6510_t *p, uint64_t pins);
uint8_t  m6510_port_pins(const m6510_t *p);   /* current P0..P5 levels */
uint16_t m6510_pc(const m6510_t *p);
uint8_t  m6510_a(const m6510_t *p);
uint8_t  m6510_x(const m6510_t *p);
uint8_t  m6510_y(const m6510_t *p);
uint8_t  m6510_s(const m6510_t *p);
uint8_t  m6510_p(const m6510_t *p);
