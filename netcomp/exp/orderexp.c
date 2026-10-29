/* orderexp.c - is netsim's stable state independent of evaluation order?
 *
 * Runs harness netcpu (reference) and a second netcpu whose netsim is
 * build/ordersim.c (tools/mkordersim.py: netsim with the nodes of every
 * pass processed in another order), starting from the same state, on the
 * same bus.  After every cycle all nodes are compared; on a difference the
 * second engine is resynchronised from the reference (unless NORESYNC is
 * set), so each count is a fresh order effect.
 *
 *   orderexp POLICY CYCLES SEED rom|random      POLICY 1 = shuffle, 2 = reverse
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netcpu.h"
#include "../../harness/gen/netlist6502.h"

void ocpu_init(netcpu_t *c);
uint64_t ocpu_tick(netcpu_t *c, uint64_t pins);
extern int osim_policy;
void osim_seed(uint64_t s);
int osim_state_load(netsim_t *s, const void *buf, size_t len);

static uint64_t rs = 1;
static uint64_t rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() >> 33) % n; }
static int is_jam(uint8_t op) { return (op & 0x1F) == 0x12 || op == 0x02 || op == 0x22 || op == 0x42 || op == 0x62; }
static const char *name(int n) {
    for (int i = 0; i < NET6502_NAMES; i++) if (net6502_names[i].node == n) return net6502_names[i].name;
    return "unnamed";
}
static void copy_state(netcpu_t *dst, const netcpu_t *src) {
    size_t n = netsim_state_size(src->sim);
    void *buf = malloc(n);
    netsim_state_save(src->sim, buf);
    if (osim_state_load(dst->sim, buf, n)) { fprintf(stderr, "state load failed\n"); exit(1); }
    free(buf);
    dst->prev_read = src->prev_read;
    dst->db_driven = src->db_driven;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: orderexp POLICY CYCLES SEED rom|random\n"); return 2; }
    osim_policy = atoi(argv[1]);
    uint64_t cycles = strtoull(argv[2], NULL, 0);
    uint64_t sd = strtoull(argv[3], NULL, 0);
    int rom = !strcmp(argv[4], "rom");
    int resync = getenv("NORESYNC") == NULL;
    rs = sd * 0x9E3779B97F4A7C15ULL + 7;
    osim_seed(sd);
    static uint8_t mem[65536], kern[8192], bas[8192];
    if (rom) {
        FILE *f = fopen("../harness/roms/kernal.901227-03.bin", "rb");
        if (!f || fread(kern, 1, 8192, f) != 8192) return 1;
        fclose(f);
        f = fopen("../harness/roms/basic.901226-01.bin", "rb");
        if (!f || fread(bas, 1, 8192, f) != 8192) return 1;
        fclose(f);
        for (int i = 0; i < 65536; i++) mem[i] = (i & 0x40) ? 0xFF : 0x00;
    } else {
        for (int i = 0; i < 65536; i++) { uint8_t v; do v = (uint8_t)(rnd() >> 40); while (is_jam(v)); mem[i] = v; }
    }
    netcpu_t A, B;
    netcpu_init(&A);
    ocpu_init(&B);
    copy_state(&B, &A);                     /* same start: the power-up settle itself is order dependent */
    static long nodediff[NET6502_NODES];
    long diffticks = 0, diffs = 0, pin_diff = 0, first_pin = -1;
    uint64_t pins = CPU_RW | CPU_RDY;
    int irq_left = 0, nmi_left = 0, rdy_left = 0;
    for (uint64_t cyc = 0; cyc < cycles; cyc++) {
        uint64_t ctl = CPU_RDY;
        if (rom) {
            if (cyc < 8) ctl |= CPU_RES;
            if (cyc > 2600000 && cyc % 16421 < 30) ctl |= CPU_IRQ;
        } else {
            ctl = 0;
            if (cyc < 3) ctl |= CPU_RES;
            if (irq_left > 0) { irq_left--; ctl |= CPU_IRQ; } else if (rn(300) == 0) irq_left = 1 + (int)rn(20);
            if (nmi_left > 0) { nmi_left--; ctl |= CPU_NMI; } else if (rn(2000) == 0) nmi_left = 1 + (int)rn(10);
            if (rdy_left > 0) rdy_left--; else { ctl |= CPU_RDY; if (rn(200) == 0) rdy_left = 1 + (int)rn(20); }
        }
        uint64_t in = (pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl;
        uint64_t oa = netcpu_tick(&A, in), ob = ocpu_tick(&B, in);
        int d = 0;
        for (int n = 0; n < NET6502_NODES; n++)
            if (netcpu_node(&A, n) != netcpu_node(&B, n)) { d++; nodediff[n]++; }
        if (oa != ob) { pin_diff++; if (first_pin < 0) first_pin = (long)cyc; }
        if (d) { diffticks++; diffs += d; if (resync) copy_state(&B, &A); }
        pins = oa;
        uint16_t a = CPU_GET_ADDR(pins);
        if (pins & CPU_RW) {
            uint8_t v;
            if (rom) v = a >= 0xE000 ? kern[a - 0xE000] : (a >= 0xA000 && a < 0xC000) ? bas[a - 0xA000] :
                         (a >= 0xD000 && a < 0xE000) ? (a == 0xD012 ? (uint8_t)((cyc / 63) % 312) : a == 0xD019 ? 0x71 :
                                                         (a == 0xDC00 || a == 0xDC01) ? 0xFF : 0) : mem[a];
            else { v = mem[a]; if ((pins & CPU_SYNC) && is_jam(v)) v = 0xEA; }
            pins = CPU_SET_DATA(pins, v);
        } else if (!rom || a < 0xD000 || a >= 0xE000) mem[a] = CPU_GET_DATA(pins);
    }
    int nodes = 0, named = 0;
    for (int n = 0; n < NET6502_NODES; n++) if (nodediff[n]) { nodes++; if (strcmp(name(n), "unnamed")) named++; }
    printf("ORDER policy=%d (%s) workload=%s cycles=%llu ticks_with_node_differences=%ld (%.1f%%) node_differences=%ld "
           "distinct_nodes=%d (named %d) pin_differences=%ld first_pin_difference=%ld%s\n",
           osim_policy, osim_policy == 1 ? "shuffled passes" : "reversed passes", rom ? "kernal-boot" : "random-soup",
           (unsigned long long)cycles, diffticks, 100.0 * diffticks / cycles, diffs, nodes, named, pin_diff, first_pin,
           resync ? "" : " (no resync)");
    printf("  most frequent:");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int n = 0; n < NET6502_NODES; n++) if (nodediff[n] && (best < 0 || nodediff[n] > nodediff[best])) best = n;
        if (best < 0) break;
        printf(" %d(%s)x%ld", best, name(best), nodediff[best]);
        nodediff[best] = 0;
    }
    printf("\n");
    return 0;
}
