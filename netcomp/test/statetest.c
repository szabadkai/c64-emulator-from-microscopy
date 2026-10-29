/* statetest.c - save states and state import.
 *  1. ccpu_state_save in the middle of a run, ccpu_state_load into a fresh
 *     core: the continuation is identical (pins every cycle, all nodes at
 *     the end) to the uninterrupted run.
 *  2. a plain struct copy is a complete state as well.
 *  3. ccpu_import_netsim: a netsim state taken after 50k cycles of netcpu,
 *     imported into ccpu, continues node-for-node with netcpu.
 * Prints STATETEST OK on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccpu.h"
#include "netcpu.h"

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static int is_jam(uint8_t op) { return (op & 0x1F) == 0x12 || op == 0x02 || op == 0x22 || op == 0x42 || op == 0x62; }

typedef struct { uint8_t mem[65536]; uint64_t pins, rng; } world_t;

static void world_init(world_t *w, uint64_t seed) {
    rs = seed;
    for (int i = 0; i < 65536; i++) { uint8_t v; do v = (uint8_t)(rnd() >> 40); while (is_jam(v)); w->mem[i] = v; }
    w->pins = CPU_RW | CPU_RDY;
    w->rng = rs;
}
static uint64_t world_ctl(world_t *w, uint64_t cyc) {
    rs = w->rng;
    uint64_t ctl = CPU_RDY;
    if (cyc < 3) ctl |= CPU_RES;
    if (rnd() % 200 == 0) ctl |= CPU_IRQ;
    if (rnd() % 900 == 0) ctl |= CPU_NMI;
    if (rnd() % 60 == 0) ctl &= ~CPU_RDY;
    w->rng = rs;
    return ctl;
}
static void world_bus(world_t *w, uint64_t out) {
    uint16_t a = CPU_GET_ADDR(out);
    if (out & CPU_RW) {
        uint8_t v = w->mem[a];
        if ((out & CPU_SYNC) && is_jam(v)) v = 0xEA;
        w->pins = CPU_SET_DATA(out, v);
    } else {
        w->mem[a] = CPU_GET_DATA(out);
        w->pins = out;
    }
}
static uint64_t step(ccpu_t *c, world_t *w, uint64_t cyc) {
    uint64_t ctl = world_ctl(w, cyc);
    uint64_t out = ccpu_tick(c, (w->pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl);
    world_bus(w, out);
    return out;
}

int main(void) {
    int fail = 0;
    static ccpu_t a, b, c2;
    static world_t wa, wb, wc;
    /* 1 + 2 */
    world_init(&wa, 12345);
    ccpu_init(&a);
    const uint64_t K = 100000, N = 200000;
    for (uint64_t i = 0; i < K; i++) step(&a, &wa, i);
    size_t sz = ccpu_state_size();
    void *buf = malloc(sz);
    ccpu_state_save(&a, buf);
    wb = wa;
    wc = wa;
    c2 = a;                                  /* struct copy */
    ccpu_init(&b);                           /* different state, then load */
    for (int i = 0; i < 1000; i++) step(&b, &wc, i);
    wc = wa;
    if (ccpu_state_load(&b, buf, sz)) { printf("state load refused\n"); fail = 1; }
    uint64_t pin_diff = 0;
    for (uint64_t i = K; i < N; i++) {
        uint64_t oa = step(&a, &wa, i), ob = step(&b, &wb, i), oc = step(&c2, &wc, i);
        if (oa != ob || oa != oc) pin_diff++;
    }
    int node_diff = 0;
    for (int n = 0; n < CC_NN; n++)
        if (ccpu_node(&a, n) != ccpu_node(&b, n) || ccpu_node(&a, n) != ccpu_node(&c2, n)) node_diff++;
    printf("savestate: saved at cycle %llu, %llu cycles continued, %llu pin differences, %d node differences (%zu bytes)\n",
           (unsigned long long)K, (unsigned long long)(N - K), (unsigned long long)pin_diff, node_diff, sz);
    if (pin_diff || node_diff) fail = 1;
    ((uint8_t *)buf)[0] ^= 1;
    if (ccpu_state_load(&b, buf, sz) == 0) { printf("corrupt state accepted\n"); fail = 1; }
    free(buf);

    /* 3: netsim state import */
    netcpu_t net;
    netcpu_init(&net);
    world_init(&wa, 777);
    for (uint64_t i = 0; i < 50000; i++) {
        uint64_t ctl = world_ctl(&wa, i);
        uint64_t out = netcpu_tick(&net, (wa.pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl);
        world_bus(&wa, out);
    }
    size_t ns = netsim_state_size(net.sim);
    void *nb = malloc(ns);
    netsim_state_save(net.sim, nb);
    ccpu_init(&a);
    if (ccpu_import_netsim(&a, nb, ns, net.prev_read, net.db_driven)) { printf("import failed\n"); fail = 1; }
    free(nb);
    int imp_nodes = 0;
    uint64_t imp_pins = 0;
    for (int n = 0; n < CC_NN; n++) if (ccpu_node(&a, n) != netcpu_node(&net, n)) imp_nodes++;
    for (uint64_t i = 50000; i < 100000; i++) {
        uint64_t ctl = world_ctl(&wa, i);
        uint64_t in = (wa.pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl;
        uint64_t o1 = netcpu_tick(&net, in), o2 = ccpu_tick(&a, in);
        if (o1 != o2) imp_pins++;
        for (int n = 0; n < CC_NN; n++) if (ccpu_node(&a, n) != netcpu_node(&net, n)) imp_nodes++;
        world_bus(&wa, o1);
    }
    printf("netsim import: 50000 cycles continued in lockstep, %llu pin differences, %d node differences\n",
           (unsigned long long)imp_pins, imp_nodes);
    if (imp_pins || imp_nodes) fail = 1;
    netcpu_free(&net);
    printf(fail ? "STATETEST FAILED\n" : "STATETEST OK\n");
    return fail;
}
