/* port6510.c - the 6510's on-chip I/O port wrapped around the netlist 6502. */
#include "port6510.h"
#include <string.h>

void m6510_init(m6510_t *p) {
    memset(p, 0, sizeof(*p));
    netcpu_init(&p->cpu);
    p->ddr = 0;       /* reset: all port pins are inputs */
    p->data = 0;
    p->ext = 0x3F;
}

void m6510_init_fast(m6510_t *p) {
    memset(p, 0, sizeof(*p));
    fastcpu_init(&p->fast);
    p->core = M6510_FAST;
    p->ext = 0x3F;
}

void m6510_init_compiled(m6510_t *p) {
    memset(p, 0, sizeof(*p));
    ccpu_init(&p->comp);
    p->core = M6510_COMPILED;
    p->ext = 0x3F;
}

void m6510_free(m6510_t *p) {
    if (p->core == M6510_NETLIST) netcpu_free(&p->cpu);
}

#define CORE_CALL(p, f) ((p)->core == M6510_FAST ? fastcpu_##f(&(p)->fast) \
                       : (p)->core == M6510_COMPILED ? ccpu_##f(&(p)->comp) : netcpu_##f(&(p)->cpu))
uint16_t m6510_pc(const m6510_t *p) { return CORE_CALL(p, pc); }
uint8_t m6510_a(const m6510_t *p) { return CORE_CALL(p, a); }
uint8_t m6510_x(const m6510_t *p) { return CORE_CALL(p, x); }
uint8_t m6510_y(const m6510_t *p) { return CORE_CALL(p, y); }
uint8_t m6510_s(const m6510_t *p) { return CORE_CALL(p, s); }
uint8_t m6510_p(const m6510_t *p) { return CORE_CALL(p, p); }

uint8_t m6510_port_pins(const m6510_t *p) {
    return (uint8_t)(((p->data & p->ddr) | (p->ext & ~p->ddr)) & 0x3F);
}

static uint8_t port_read(const m6510_t *p, uint16_t addr) {
    if (addr == 0) return p->ddr;
    /* bits 0-5: pin levels; bits 6-7 have no pins: output latch if
       configured as output, else 0 */
    return (uint8_t)(m6510_port_pins(p) | (p->data & p->ddr & 0xC0));
}

uint64_t m6510_tick(m6510_t *p, uint64_t pins) {
    p->ext = (uint8_t)((pins >> CPU_P_SHIFT) & 0x3F);
    /* the previous cycle read $00/$01: the port, not the bus, supplies data */
    if (p->port_read) pins = CPU_SET_DATA(pins, port_read(p, p->port_addr));

    switch (p->core) {
    case M6510_FAST: pins = fastcpu_tick(&p->fast, pins); break;
    case M6510_COMPILED: pins = ccpu_tick(&p->comp, pins); break;
    default: pins = netcpu_tick(&p->cpu, pins);
    }

    uint16_t addr = CPU_GET_ADDR(pins);
    p->port_read = false;
    if (addr <= 1) {
        if (pins & CPU_RW) {
            p->port_read = true;
            p->port_addr = addr;
        } else if (addr == 0) {
            p->ddr = CPU_GET_DATA(pins);
        } else {
            p->data = CPU_GET_DATA(pins);
        }
    }
    pins = (pins & ~CPU_P_MASK) | ((uint64_t)m6510_port_pins(p) << CPU_P_SHIFT);
    return pins;
}
