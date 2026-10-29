/* lstep.c - lcpu (levelized core) vs netsim, plus the nodestep checks of ccpu;
 * derived from nodestep.c - node-exact lockstep: harness netsim (reference) vs. the
 * compiled core, every node compared after every recalc of every half
 * cycle; fastcpu rides along as a bus-level secondary check.
 *
 * The scenarios (opcodes, jam, irq, rdy, alu, reset, random, rom) are those
 * of ../../cpu6510/lockstep.c, included unchanged: its netcpu_init/tick/free
 * calls are bound to the wrappers below, which run the reference netcpu and
 * a ccpu_t side by side.
 *
 *   nodestep <lockstep args...>        e.g.  nodestep random 1 3000000
 *   NODESTEP_TICK=1 nodestep ...       compare after each full tick, using the
 *                                      real netcpu_tick()/ccpu_tick() (checks
 *                                      that the per-recalc replica below is
 *                                      the same as netcpu_tick)
 *
 * Extra output line:
 *   NODES <test> halfcycles=H recalcs=R nodes_compared=N node_mismatches=M bus_mismatches=B
 * A node mismatch is listed (node id, name, class, values, phase, cycle),
 * then the compiled core is resynchronised from the netsim state so the run
 * can go on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netcpu.h"
#include "netsim.h"
#include "ccpu.h"
#include "lcpu.h"

static ccpu_t CC;
static lcpu_t LC;
static int lc_noresync = -1;
static uint64_t lc_cmp_recalcs, lc_node_mism, lc_mism_events, lc_pin_mism, lc_class[4];
static uint32_t lc_per_node[CC_NN][4];
static const char *const CLASS_NAME[4] = { "floating (stored charge)", "pulled up", "pad-driven", "driven to VSS/VCC" };
static int tick_mode = -1;
static const char *cur_test = "?";
static uint64_t ns_halfcycles, ns_recalcs, ns_nodes_cmp, ns_node_mism, ns_mism_events, ns_bus_mism;
static uint64_t ns_ticks;
static uint32_t mism_per_node[CC_NN];

static const char *node_name(int n) {
    static char buf[64];
    const char *nm = ccpu_node_name(n);
    if (nm) snprintf(buf, sizeof buf, "%s", nm); else snprintf(buf, sizeof buf, "unnamed");
    return buf;
}

static void resync(netcpu_t *ref) {
    size_t n = netsim_state_size(ref->sim);
    void *buf = malloc(n);
    netsim_state_save(ref->sim, buf);
    if (ccpu_import_netsim(&CC, buf, n, ref->prev_read, ref->db_driven)) {
        fprintf(stderr, "nodestep: netsim state import failed\n");
        exit(1);
    }
    free(buf);
}

/* lcpu vs netsim after a recalc; mismatches are classified by the drive of
   the node's group in netsim's state (via the exact mirror CC) */
static void lc_compare(netcpu_t *ref, const char *phase) {
    if (lc_noresync < 0) lc_noresync = getenv("LSTEP_NORESYNC") != NULL;
    int bad = 0;
    lc_cmp_recalcs++;
    for (int n = 0; n < CC_NN; n++) {
        int a = netsim_get(ref->sim, n), b = lcpu_node(&LC, n);
        if (a == b) continue;
        int cl = ccpu_group_drive(&CC, n);
        lc_class[cl]++;
        lc_per_node[n][cl]++;
        if (lc_mism_events < 10 && bad < 8)
            printf("  lcpu: cycle %llu after %s: node %d (%s) netsim=%d lcpu=%d [%s]\n", (unsigned long long)ns_ticks,
                   phase, n, node_name(n), a, b, CLASS_NAME[cl]);
        bad++;
    }
    if (bad) {
        lc_node_mism += (uint64_t)bad;
        lc_mism_events++;
        if (!lc_noresync) {
            uint8_t flags[CC_NN];
            for (int n = 0; n < CC_NN; n++) flags[n] = CC.f[n];
            lcpu_set_nodes(&LC, flags, flags);
        }
    }
}

/* compare every node; returns number of mismatching nodes */
static int compare(netcpu_t *ref, const char *phase) {
    int bad = 0;
    for (int n = 0; n < CC_NN; n++) {
        int a = netsim_get(ref->sim, n), b = ccpu_node(&CC, n);
        if (a != b) {
            if (ns_mism_events < 20 && bad < 12)
                printf("*** NODE MISMATCH (%s) cycle %llu after %s: node %d (%s) netsim=%d compiled=%d\n",
                       cur_test, (unsigned long long)ns_ticks, phase, n, node_name(n), a, b);
            mism_per_node[n]++;
            bad++;
        }
    }
    ns_nodes_cmp += CC_NN;
    ns_recalcs++;
    if (bad) {
        ns_node_mism += (uint64_t)bad;
        ns_mism_events++;
        resync(ref);
    }
    return bad;
}

static void ns_init(netcpu_t *c) {
    netcpu_init(c);
    ccpu_init(&CC);
    lcpu_init(&LC);
    compare(c, "power-up (netcpu_init)");
    lc_compare(c, "power-up (netcpu_init)");
    if (CC.prev_read != c->prev_read || CC.db_driven != c->db_driven) printf("*** wrapper state differs after init\n");
}

static void ns_free(netcpu_t *c) { netcpu_free(c); }

static uint32_t rbus(netsim_t *s, const int *ids, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
        if (ids[i] >= 0 && netsim_get(s, ids[i])) v |= 1u << i;
    return v;
}

/* replica of netcpu_tick() for the reference, with the same steps on the
   compiled core and a full node comparison after each recalc */
static uint64_t half_tick(netcpu_t *c, uint64_t pins) {
    netsim_t *s = c->sim;
    if (c->prev_read != CC.prev_read || c->db_driven != CC.db_driven) {
        printf("*** wrapper state differs (prev_read %d/%d db_driven %d/%d)\n", c->prev_read, CC.prev_read,
               c->db_driven, CC.db_driven);
        CC.prev_read = c->prev_read; CC.db_driven = c->db_driven;
    }
    if (c->prev_read) {
        int d = CPU_GET_DATA(pins);
        if (d != c->db_driven) {
            for (int i = 0; i < 8; i++) netsim_drive(s, c->n_db[i], (d >> i) & 1);
            c->db_driven = d;
            netsim_recalc(s);
            for (int i = 0; i < 8; i++) ccpu_drive(&CC, c->n_db[i], (d >> i) & 1);
            CC.db_driven = d;
            ccpu_recalc(&CC);
            compare(c, "data pads driven (end of PHI2)");
            if (LC.prev_read) lcpu_step_db(&LC, d);
            lc_compare(c, "data pads driven (end of PHI2)");
        }
    }
    netsim_drive(s, c->n_clk0, 0); netsim_recalc(s);
    ccpu_drive(&CC, c->n_clk0, 0); ccpu_recalc(&CC);
    compare(c, "CLK0 fall (PHI1)");
    lcpu_step_phi1(&LC);
    lc_compare(c, "CLK0 fall (PHI1)");
    ns_halfcycles++;

    netsim_drive(s, c->n_rdy, (pins & CPU_RDY) ? 1 : 0);
    netsim_drive(s, c->n_irq, (pins & CPU_IRQ) ? 0 : 1);
    netsim_drive(s, c->n_nmi, (pins & CPU_NMI) ? 0 : 1);
    netsim_drive(s, c->n_res, (pins & CPU_RES) ? 0 : 1);
    netsim_recalc(s);
    ccpu_drive(&CC, c->n_rdy, (pins & CPU_RDY) ? 1 : 0);
    ccpu_drive(&CC, c->n_irq, (pins & CPU_IRQ) ? 0 : 1);
    ccpu_drive(&CC, c->n_nmi, (pins & CPU_NMI) ? 0 : 1);
    ccpu_drive(&CC, c->n_res, (pins & CPU_RES) ? 0 : 1);
    ccpu_recalc(&CC);
    compare(c, "RDY/IRQ/NMI/RES applied (PHI1)");
    lcpu_step_ctl(&LC, pins);
    lc_compare(c, "RDY/IRQ/NMI/RES applied (PHI1)");

    netsim_drive(s, c->n_clk0, 1); netsim_recalc(s);
    ccpu_drive(&CC, c->n_clk0, 1); ccpu_recalc(&CC);
    compare(c, "CLK0 rise (PHI2)");
    lcpu_step_phi2(&LC);
    lc_compare(c, "CLK0 rise (PHI2)");
    ns_halfcycles++;

    uint64_t out = pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC);
    out |= rbus(s, c->n_ab, 16);
    int rw = netsim_get(s, c->n_rw);
    if (rw) out |= CPU_RW;
    if (netsim_get(s, c->n_sync)) out |= CPU_SYNC;
    if (!rw) { out = CPU_SET_DATA(out, rbus(s, c->n_db, 8)); c->db_driven = -1; CC.db_driven = -1; }
    {
        uint64_t lo = lcpu_outputs(&LC, pins);
        if (lo != out) {
            lc_pin_mism++;
            if (lc_pin_mism < 10) printf("  lcpu PIN MISMATCH cycle %llu: netsim %04X %c %02X  lcpu %04X %c %02X\n",
                (unsigned long long)ns_ticks, CPU_GET_ADDR(out), (out & CPU_RW) ? 'R' : 'W', CPU_GET_DATA(out),
                CPU_GET_ADDR(lo), (lo & CPU_RW) ? 'R' : 'W', CPU_GET_DATA(lo));
        }
        if (!lc_noresync) { LC.prev_read = rw != 0; LC.db_driven = c->db_driven; }
    }
    c->prev_read = rw != 0;
    CC.prev_read = rw != 0;
    c->cycles++;
    CC.cycles++;
    return out;
}

static uint64_t ns_tick(netcpu_t *c, uint64_t pins) {
    if (tick_mode < 0) tick_mode = getenv("NODESTEP_TICK") != NULL;
    ns_ticks++;
    uint64_t out;
    if (tick_mode) {
        out = netcpu_tick(c, pins);
        uint64_t o2 = ccpu_tick(&CC, pins);
        compare(c, "tick");
        ns_halfcycles += 2;
        if (o2 != out) {
            ns_bus_mism++;
            if (ns_bus_mism < 10) printf("*** PIN MISMATCH cycle %llu: netcpu %016llx ccpu %016llx\n",
                                         (unsigned long long)ns_ticks, (unsigned long long)out, (unsigned long long)o2);
        }
        if (CC.prev_read != c->prev_read || CC.db_driven != c->db_driven) {
            ns_bus_mism++;
            printf("*** wrapper state differs after tick\n");
            CC.prev_read = c->prev_read; CC.db_driven = c->db_driven;
        }
    } else {
        out = half_tick(c, pins);
    }
    return out;
}

#define netcpu_init ns_init
#define netcpu_free ns_free
#define netcpu_tick ns_tick
#define main lockstep_main
#include "../../cpu6510/lockstep.c"
#undef main
#undef netcpu_init
#undef netcpu_free
#undef netcpu_tick

static void at_exit_report(void) {
    printf("LCPU %s recalcs=%llu node_mismatches=%llu mismatch_events=%llu floating=%llu pulled_up=%llu pad=%llu driven=%llu pin_mismatches=%llu%s\n",
           cur_test, (unsigned long long)lc_cmp_recalcs, (unsigned long long)lc_node_mism, (unsigned long long)lc_mism_events,
           (unsigned long long)lc_class[0], (unsigned long long)lc_class[1], (unsigned long long)lc_class[2], (unsigned long long)lc_class[3],
           (unsigned long long)lc_pin_mism, lc_noresync ? " (no resync)" : "");
    if (lc_node_mism) {
        printf("lcpu mismatching nodes (node:count by class F/U/P/D):");
        for (int n = 0; n < CC_NN; n++) {
            uint32_t *k = lc_per_node[n];
            if (k[0] | k[1] | k[2] | k[3]) printf(" %d(%s):%u/%u/%u/%u", n, node_name(n), k[0], k[1], k[2], k[3]);
        }
        printf("\n");
    }
    printf("NODES %s halfcycles=%llu recalcs=%llu nodes_compared=%llu node_mismatches=%llu mismatch_events=%llu bus_mismatches=%llu\n",
           cur_test, (unsigned long long)ns_halfcycles, (unsigned long long)ns_recalcs, (unsigned long long)ns_nodes_cmp,
           (unsigned long long)ns_node_mism, (unsigned long long)ns_mism_events, (unsigned long long)ns_bus_mism);
    if (ns_node_mism) {
        printf("mismatching nodes:");
        for (int n = 0; n < CC_NN; n++) if (mism_per_node[n]) printf(" %d(%s)x%u", n, node_name(n), mism_per_node[n]);
        printf("\n");
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) cur_test = argv[1];
    atexit(at_exit_report);
    int r = lockstep_main(argc, argv);
    return r;
}
