/* netcpu.c - binds the generic switch-level engine (netsim.c) to the
 * visual6502 NMOS 6502 netlist and gives it a cycle-level pin interface. */
#include "netcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gen/netlist6502.h"

int netcpu_node_id(const char *name) {
    for (int i = 0; i < NET6502_NAMES; i++)
        if (strcmp(net6502_names[i].name, name) == 0) return net6502_names[i].node;
    return -1;
}

static int must(const char *name) {
    int n = netcpu_node_id(name);
    if (n < 0) { fprintf(stderr, "netcpu: node '%s' not in netlist\n", name); exit(1); }
    return n;
}

static void bus_ids(int *ids, const char *prefix, int count) {
    char buf[32];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof buf, "%s%d", prefix, i);
        ids[i] = netcpu_node_id(buf);   /* may be -1 (e.g. p5) */
    }
}

static uint32_t read_bus(const netcpu_t *c, const int *ids, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
        if (ids[i] >= 0 && netsim_get(c->sim, ids[i])) v |= 1u << i;
    return v;
}

static void half_clock(netcpu_t *c, int level) {
    netsim_drive(c->sim, c->n_clk0, level);
    netsim_recalc(c->sim);
}

void netcpu_init(netcpu_t *c) {
    memset(c, 0, sizeof(*c));
    int vss = must("vss"), vcc = must("vcc");
    c->sim = netsim_create(NET6502_NODES, NET6502_TRANSISTORS, net6502_trans,
                           net6502_pullups, NET6502_PULLUPS, vss, vcc);
    bus_ids(c->n_ab, "ab", 16);
    bus_ids(c->n_db, "db", 8);
    c->n_rw = must("rw"); c->n_sync = must("sync"); c->n_clk0 = must("clk0");
    c->n_rdy = must("rdy"); c->n_irq = must("irq"); c->n_nmi = must("nmi");
    c->n_res = must("res"); c->n_so = must("so");
    bus_ids(c->n_a, "a", 8); bus_ids(c->n_x, "x", 8); bus_ids(c->n_y, "y", 8);
    bus_ids(c->n_s, "s", 8); bus_ids(c->n_p, "p", 8); bus_ids(c->n_pcl, "pcl", 8);
    bus_ids(c->n_pch, "pch", 8); bus_ids(c->n_ir, "ir", 8);

    /* power-up: /RES low, CLK0 high, RDY high, SO/IRQ/NMI inactive (high),
       data bus pulled to 0 */
    netsim_t *s = c->sim;
    netsim_drive(s, c->n_res, 0);
    netsim_drive(s, c->n_clk0, 1);
    netsim_drive(s, c->n_rdy, 1);
    netsim_drive(s, c->n_so, 1);
    netsim_drive(s, c->n_irq, 1);
    netsim_drive(s, c->n_nmi, 1);
    for (int i = 0; i < 8; i++) netsim_drive(s, c->n_db[i], 0);
    c->db_driven = 0;
    netsim_stabilize(s);
    /* clock a few cycles with /RES held low so internal state settles */
    for (int i = 0; i < 8; i++) { half_clock(c, 0); half_clock(c, 1); }
    c->prev_read = true;
}

void netcpu_free(netcpu_t *c) { netsim_destroy(c->sim); c->sim = NULL; }

uint64_t netcpu_tick(netcpu_t *c, uint64_t pins) {
    netsim_t *s = c->sim;

    /* --- end of previous PHI2: data for the previous read is on the pads
       before the falling clock edge latches it */
    if (c->prev_read) {
        int d = CPU_GET_DATA(pins);
        if (d != c->db_driven) {
            for (int i = 0; i < 8; i++) netsim_drive(s, c->n_db[i], (d >> i) & 1);
            c->db_driven = d;
            netsim_recalc(s);
        }
    }

    /* --- PHI1: CLK0 falls; CPU latches data, puts out the new address */
    half_clock(c, 0);

    /* control inputs of THIS cycle, valid during its PHI1 (6502 datasheet:
       RDY low during PHI1 halts the CPU on the current read cycle) */
    netsim_drive(s, c->n_rdy, (pins & CPU_RDY) ? 1 : 0);
    netsim_drive(s, c->n_irq, (pins & CPU_IRQ) ? 0 : 1);
    netsim_drive(s, c->n_nmi, (pins & CPU_NMI) ? 0 : 1);
    netsim_drive(s, c->n_res, (pins & CPU_RES) ? 0 : 1);
    netsim_recalc(s);   /* no-op when nothing changed */

    /* --- PHI2: CLK0 rises; on a write the CPU drives the data pads */
    half_clock(c, 1);

    uint64_t out = pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC);
    out |= read_bus(c, c->n_ab, 16);
    int rw = netsim_get(s, c->n_rw);
    if (rw) out |= CPU_RW;
    if (netsim_get(s, c->n_sync)) out |= CPU_SYNC;
    if (!rw) {
        out = CPU_SET_DATA(out, read_bus(c, c->n_db, 8));
        c->db_driven = -1;   /* pads now hold CPU-driven values; re-drive next read */
    }
    c->prev_read = rw != 0;
    c->cycles++;
    return out;
}

int     netcpu_node(const netcpu_t *c, int node) { return netsim_get(c->sim, node); }
uint8_t netcpu_a(const netcpu_t *c) { return read_bus(c, c->n_a, 8); }
uint8_t netcpu_x(const netcpu_t *c) { return read_bus(c, c->n_x, 8); }
uint8_t netcpu_y(const netcpu_t *c) { return read_bus(c, c->n_y, 8); }
uint8_t netcpu_s(const netcpu_t *c) { return read_bus(c, c->n_s, 8); }
uint8_t netcpu_ir(const netcpu_t *c) { return read_bus(c, c->n_ir, 8); }
uint16_t netcpu_pc(const netcpu_t *c) {
    return (uint16_t)(read_bus(c, c->n_pcl, 8) | (read_bus(c, c->n_pch, 8) << 8));
}
uint8_t netcpu_p(const netcpu_t *c) {
    /* there is no physical B or bit-5 flag: report bit5=1, bit4=0 */
    uint8_t p = (uint8_t)read_bus(c, c->n_p, 8);
    return (uint8_t)((p & 0xCF) | 0x20);
}
int netcpu_transistors(const netcpu_t *c) { return netsim_num_transistors(c->sim); }
