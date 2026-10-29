/* lcpu.c - runtime of the levelized compiled 6502 (code from ncc.py,
 * tools/lgen.py).  See lcpu.h and README.md. */
#include "lcpu.h"
#include <string.h>
#include "ccpu.h"

#define LC_LOOP_LIMIT 1000

/* components whose nodes gate several of their own transistors (the ALU):
   tables + a group-by-group evaluator */
typedef struct {
    int16_t node;
    uint8_t gc, pc;          /* a transistor to VSS / VCC that is on for the whole phase */
    int8_t padi;             /* pad index or -1 */
    uint8_t pu;              /* netlist pull-up */
    uint8_t src;             /* gates a transistor of this component */
    uint8_t ngnd, npwr;
    int16_t gnd[LC_MAXRAIL], pwr[LC_MAXRAIL];
} lc_sg_node_t;
typedef struct { int16_t gate, a, b; } lc_sg_chan_t;   /* gate -1: on for the whole phase */
typedef struct { int n, nch; const lc_sg_node_t *nodes; const lc_sg_chan_t *chans; } lc_sg_t;

static inline int uf_find(uint8_t *r, int x) {
    while (r[x] != x) { r[x] = r[r[x]]; x = r[x]; }
    return x;
}

/* Evaluate a component whose nodes gate its own transistors.  One round:
   all groups (connected sets through conducting transistors) are computed
   from the current state.  Groups that contain no node acting as a gate
   for this component and changing value cannot influence any other group,
   so assigning all of them together is the same as evaluating them one
   after the other; of the groups that do contain such a changing node only
   the first is assigned (it is then the last group evaluated, seeing the
   others' new values), the rest wait for the next round.  Rounds repeat
   until nothing changes, so every assignment is one of netsim's group
   evaluations, in some order. */
static unsigned sg_eval(uint8_t *restrict v, const uint8_t *restrict pad, const lc_sg_t *g, lcpu_stats_t *st);

#include "../gen/lcpu_net.inc"

static unsigned sg_eval(uint8_t *restrict v, const uint8_t *restrict pad, const lc_sg_t *g, lcpu_stats_t *st) {
    unsigned any = 0;
    uint8_t root[LC_MAXSG], held[LC_MAXSG];
    unsigned gm[LC_MAXSG], m[LC_MAXSG];
    for (int round = 0; round < LC_LOOP_LIMIT; round++) {
        st->sg_rounds++;
        const int n = g->n;
        for (int i = 0; i < n; i++) {
            const lc_sg_node_t *x = &g->nodes[i];
            unsigned gnd = x->gc, pwr = x->pc;
            for (int k = 0; k < x->ngnd; k++) gnd |= v[x->gnd[k]];
            for (int k = 0; k < x->npwr; k++) pwr |= v[x->pwr[k]];
            m[i] = (gnd << 5) | (pwr << 4) | (x->padi >= 0 ? pad[x->padi] : (unsigned)x->pu << 2) | ((unsigned)v[x->node] << 1);
            root[i] = (uint8_t)i;
            gm[i] = 0;
            held[i] = 0;
        }
        for (int k = 0; k < g->nch; k++) {
            const lc_sg_chan_t *t = &g->chans[k];
            if (t->gate >= 0 && !v[t->gate]) continue;
            int a = uf_find(root, t->a), b = uf_find(root, t->b);
            if (a != b) root[a] = (uint8_t)b;
        }
        for (int i = 0; i < n; i++) gm[uf_find(root, i)] |= m[i];
        int first = -1;
        for (int i = 0; i < n; i++) {
            if (!g->nodes[i].src) continue;
            int r = uf_find(root, i);
            if (DEC[gm[r]] != v[g->nodes[i].node]) {
                if (first < 0) first = r;
                else if (r != first) held[r] = 1;
            }
        }
        unsigned chg = 0;
        for (int i = 0; i < n; i++) {
            int r = uf_find(root, i);
            if (held[r]) continue;
            uint8_t nv = DEC[gm[r]];
            chg |= nv ^ v[g->nodes[i].node];
            v[g->nodes[i].node] = nv;
        }
        any |= chg;
        if (!chg) return any;
    }
    st->loop_limit_hits++;
    return any;
}

static const int16_t BUS_AB[16] = CC_BUS_AB, BUS_DB[8] = CC_BUS_DB;
static const int16_t BUS_A[8] = CC_BUS_A, BUS_X[8] = CC_BUS_X, BUS_Y[8] = CC_BUS_Y, BUS_S[8] = CC_BUS_S,
                     BUS_P[8] = CC_BUS_P, BUS_PCL[8] = CC_BUS_PCL, BUS_PCH[8] = CC_BUS_PCH, BUS_IR[8] = CC_BUS_IR;
enum { PAD_CLK0 = 0, PAD_RDY, PAD_IRQ, PAD_NMI, PAD_RES, PAD_SO, PAD_DB0 };

static uint32_t read_bus(const lcpu_t *c, const int16_t *ids, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
        if (ids[i] >= 0 && c->v[ids[i]]) v |= 1u << i;
    return v;
}

void lcpu_set_nodes(lcpu_t *c, const uint8_t *values, const uint8_t *flags) {
    for (int n = 0; n < CC_NN; n++) c->v[n] = values[n] & 1;
    for (int i = 0; i < LC_NPADS; i++) {
        uint8_t f = flags[LC_PADS[i]];
        c->pad[i] = (f & 4) ? 8 : (f & 2) ? 4 : 0;    /* netsim: pull-down wins over pull-up */
    }
}

void lcpu_init(lcpu_t *c) {
    /* power-up = netcpu_init, done by the exact engine (the settling from
       all-zero nodes is itself order dependent, so it has to be netsim's) */
    static ccpu_t e;
    ccpu_init(&e);
    memset(c, 0, sizeof *c);
    lcpu_set_nodes(c, e.f, e.f);
    c->prev_read = e.prev_read;
    c->db_driven = e.db_driven;
}

void lcpu_free(lcpu_t *c) { (void)c; }

void lcpu_step_db(lcpu_t *c, int d) {
    for (int i = 0; i < 8; i++) c->pad[PAD_DB0 + i] = ((d >> i) & 1) ? 4 : 8;
    c->db_driven = d;
    c->st.cones++;
    lc_cone_db(c->v, c->pad, &c->st);
}

void lcpu_step_phi1(lcpu_t *c) {
    c->pad[PAD_CLK0] = 8;
    c->st.sweeps++;
    lc_sweep_phi1(c->v, c->pad, &c->st);
}

void lcpu_step_ctl(lcpu_t *c, uint64_t pins) {
    uint8_t rdy = (pins & CPU_RDY) ? 4 : 8, irq = (pins & CPU_IRQ) ? 8 : 4;
    uint8_t nmi = (pins & CPU_NMI) ? 8 : 4, res = (pins & CPU_RES) ? 8 : 4;
    if (rdy == c->pad[PAD_RDY] && irq == c->pad[PAD_IRQ] && nmi == c->pad[PAD_NMI] && res == c->pad[PAD_RES]) return;
    c->pad[PAD_RDY] = rdy; c->pad[PAD_IRQ] = irq; c->pad[PAD_NMI] = nmi; c->pad[PAD_RES] = res;
    c->st.cones++;
    lc_cone_ctl(c->v, c->pad, &c->st);
}

void lcpu_step_phi2(lcpu_t *c) {
    c->pad[PAD_CLK0] = 4;
    c->st.sweeps++;
    lc_sweep_phi2(c->v, c->pad, &c->st);
}

uint64_t lcpu_outputs(lcpu_t *c, uint64_t pins) {
    uint64_t out = pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC);
    out |= read_bus(c, BUS_AB, 16);
    int rw = c->v[CC_N_RW];
    if (rw) out |= CPU_RW;
    if (c->v[CC_N_SYNC]) out |= CPU_SYNC;
    if (!rw) {
        out = CPU_SET_DATA(out, read_bus(c, BUS_DB, 8));
        c->db_driven = -1;
    }
    c->prev_read = rw != 0;
    c->cycles++;
    return out;
}

uint64_t lcpu_tick(lcpu_t *c, uint64_t pins) {
    if (c->prev_read) {
        int d = CPU_GET_DATA(pins);
        if (d != c->db_driven) lcpu_step_db(c, d);
    }
    lcpu_step_phi1(c);
    lcpu_step_ctl(c, pins);
    lcpu_step_phi2(c);
    return lcpu_outputs(c, pins);
}

uint8_t lcpu_a(const lcpu_t *c) { return (uint8_t)read_bus(c, BUS_A, 8); }
uint8_t lcpu_x(const lcpu_t *c) { return (uint8_t)read_bus(c, BUS_X, 8); }
uint8_t lcpu_y(const lcpu_t *c) { return (uint8_t)read_bus(c, BUS_Y, 8); }
uint8_t lcpu_s(const lcpu_t *c) { return (uint8_t)read_bus(c, BUS_S, 8); }
uint8_t lcpu_ir(const lcpu_t *c) { return (uint8_t)read_bus(c, BUS_IR, 8); }
uint16_t lcpu_pc(const lcpu_t *c) { return (uint16_t)(read_bus(c, BUS_PCL, 8) | (read_bus(c, BUS_PCH, 8) << 8)); }
uint8_t lcpu_p(const lcpu_t *c) { return (uint8_t)((read_bus(c, BUS_P, 8) & 0xCF) | 0x20); }
const lcpu_stats_t *lcpu_stats(const lcpu_t *c) { return &c->st; }

#define LCPU_STATE_MAGIC 0x4C435055u   /* "LCPU" */
size_t lcpu_state_size(void) { return 8 + sizeof(lcpu_t); }
void lcpu_state_save(const lcpu_t *c, void *buf) {
    uint32_t hdr[2] = { LCPU_STATE_MAGIC, (uint32_t)sizeof(lcpu_t) };
    memcpy(buf, hdr, 8);
    memcpy((uint8_t *)buf + 8, c, sizeof *c);
}
int lcpu_state_load(lcpu_t *c, const void *buf, size_t len) {
    uint32_t hdr[2];
    if (len != lcpu_state_size()) return -1;
    memcpy(hdr, buf, 8);
    if (hdr[0] != LCPU_STATE_MAGIC || hdr[1] != sizeof(lcpu_t)) return -1;
    memcpy(c, (const uint8_t *)buf + 8, sizeof *c);
    return 0;
}
