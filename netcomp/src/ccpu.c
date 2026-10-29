/* ccpu.c - runtime of the compiled 6502 netlist (code from ncc.py).
 *
 * The evaluation order is exactly netsim's (harness/src/netsim.c): passes
 * over a de-duplicated queue, nodes processed in queue order, group walks
 * over the conducting transistors in the order netsim keeps them, every
 * change switching its transistors immediately.  netsim's stable state is
 * order dependent (README.md, "Order dependence"), so this is the only way
 * to be node-for-node identical to it. */
#include "ccpu.h"
#include <string.h>

#ifdef CCPU_STATS
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

#define LOOP_LIMIT 1000

/* netsim's queue(): append n to the list being filled unless it is
   already in it.  Branch-free: always write, advance only if new. */
#define QF(n) do {                                            \
        uint32_t m_ = c->omark[n];                            \
        qout[nout] = (uint16_t)(n);                           \
        nout += (m_ != st);                                   \
        c->omark[n] = st;                                     \
    } while (0)
#define QC(n, cond) do {                                      \
        uint32_t m_ = c->omark[n];                            \
        int k_ = (cond) & (m_ != st);                         \
        qout[nout] = (uint16_t)(n);                           \
        nout += k_;                                           \
        c->omark[n] = k_ ? st : m_;                           \
    } while (0)

/* netsim simple_value(): ngnd ? 0 : npwr ? 1 : pull-down ? 0 : pull-up ? 1 : charge,
   as a table on (ngnd != 0, npwr != 0, flags) */
static const uint8_t SVT[32] = {
    0, 1, 1, 1, 0, 0, 0, 0,  1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0 };
#define SVTAB(f) SVT[(f) & 7]
#define SVNP(c, d) SVT[(((c)->ngnd[d] != 0) << 4) | ((c)->f[d] & 7)]
#define SV(c, d) SVT[(((c)->ngnd[d] != 0) << 4) | (((c)->npwr[d] != 0) << 3) | ((c)->f[d] & 7)]

/* netsim link_slot()/unlink_slot(): prepend to / remove from a node's list */
#define LINK(sl, node) do {                                   \
        int H_ = CC_HB + (node), h_ = c->nxt[H_];             \
        c->nxt[sl] = (uint16_t)h_; c->prv[sl] = (uint16_t)H_; \
        c->prv[h_] = (uint16_t)(sl); c->nxt[H_] = (uint16_t)(sl); \
    } while (0)
#define UNLINK(sl) do {                                       \
        int p_ = c->prv[sl], n_ = c->nxt[sl];                 \
        c->nxt[p_] = (uint16_t)n_; c->prv[n_] = (uint16_t)p_; \
    } while (0)

#include "../gen/ccpu_net.inc"
#ifndef CCPU_UNROLLED
/* netsim's set_node(): node m changed to v -- switch the transistors it
   gates and queue what may now change, in netsim's order.  Driven by the
   per-node fan-out tables ncc.py generated.  (-DCCPU_UNROLLED uses the
   same operations emitted as straight-line C per node instead: 490 KB of
   code, which thrashes the 192 KB L1 I-cache and runs ~20% slower.) */
static void set_node(ccpu_t *restrict c, int m, int v) {
    uint16_t *restrict qout = c->q[c->qo];
    int nout = c->nout;
    const uint32_t st = c->ostamp;
    c->f[m] ^= 1;
    STAT(c->st.changes++);
    const uint16_t *restrict p = CC_FO + CC_FO_OFF[(m << 1) | v];
    int n = *p++;
    if (v) {
        for (; n > 0; n--, p += 3) {
            int sl = p[0], c1 = p[1], c2 = p[2];
            LINK(sl, c1); LINK(sl + 1, c2);
            QC(c1, (c->f[c1] ^ c->f[c2]) & 1);
        }
        n = *p++;
        for (; n > 0; n--, p++) {
            int d = *p & 0x7FFF, s = *p >> 15;
            int old = c->ngnd[d]++;
            QC(d, s ? (c->f[d] & 1) : (old == 0));
        }
        n = *p++;
        for (; n > 0; n--, p++) {
            int d = *p & 0x7FFF, s = *p >> 15;
            int old = c->npwr[d]++;
            QC(d, s ? ((c->ngnd[d] == 0) != (c->f[d] & 1)) : (old == 0));
        }
    } else {
        for (; n > 0; n--, p += 3) { UNLINK(p[0]); UNLINK(p[0] + 1); }
        n = *p++;
        for (; n > 0; n--, p++) {
            int d = *p & 0x7FFF, s = *p >> 15;
            int now = --c->ngnd[d];
            QC(d, s ? (SV(c, d) != (c->f[d] & 1)) : (now == 0));
        }
        n = *p++;
        for (; n > 0; n--, p++) {
            int d = *p & 0x7FFF, s = *p >> 15;
            int now = --c->npwr[d];
            QC(d, s ? (SV(c, d) != (c->f[d] & 1)) : (now == 0));
        }
        n = *p++;
        for (; n > 0; n--, p++) QF(*p);
    }
    c->nout = nout;
}
#endif

static const int16_t BUS_AB[16] = CC_BUS_AB, BUS_DB[8] = CC_BUS_DB;
static const int16_t BUS_A[8] = CC_BUS_A, BUS_X[8] = CC_BUS_X, BUS_Y[8] = CC_BUS_Y, BUS_S[8] = CC_BUS_S,
                     BUS_P[8] = CC_BUS_P, BUS_PCL[8] = CC_BUS_PCL, BUS_PCH[8] = CC_BUS_PCH, BUS_IR[8] = CC_BUS_IR;

#define Q(n) do { uint16_t *restrict qout = c->q[c->qo]; int nout = c->nout; const uint32_t st = c->ostamp; QF(n); c->nout = nout; } while (0)

static inline void recalc_group(ccpu_t *restrict c, int n, uint32_t gs) {
    uint16_t *restrict group = c->group;
    int ng = 1;
    unsigned gnd = c->ngnd[n], pwr = c->npwr[n];
    uint8_t acc = c->f[n];
    group[0] = (uint16_t)n;
    c->gmark[n] = gs;
    for (int i = 0; i < ng; i++) {
        int m = group[i];
        for (int sl = c->nxt[CC_HB + m]; sl; sl = c->nxt[sl]) {
            int o = CC_SOTHER1[sl];
            if (c->gmark[o] == gs) continue;
            c->gmark[o] = gs;
            group[ng++] = (uint16_t)o;
            acc |= c->f[o];
            gnd |= c->ngnd[o];
            pwr |= c->npwr[o];
        }
    }
    int v;
    if (gnd) v = 0;
    else if (pwr) v = 1;
    else v = SVTAB(acc);
    STAT(c->st.walks++);
    STAT(c->st.group_nodes += ng);
    for (int i = 0; i < ng; i++) {
        int m = group[i];
        if ((c->f[m] & 1) != v) set_node(c, m, v);
    }
}

void ccpu_recalc(ccpu_t *c) {
    if (c->gstamp > 0xF0000000u) { memset(c->gmark, 0, sizeof c->gmark); c->gstamp = 1; }
    int pass;
    for (pass = 0; pass < LOOP_LIMIT && c->nout; pass++) {
        const uint16_t *restrict in = c->q[c->qo];
        const int nin = c->nout;
        c->qo ^= 1;
        c->nout = 0;
        if (++c->ostamp == 0) { memset(c->omark, 0, sizeof c->omark); c->ostamp = 1; }
        const uint32_t pass_start = c->gstamp;
        STAT(c->st.passes++);
        STAT(c->st.evals += (uint64_t)nin);
        int i = 0;
        for (;;) {
            /* Skip, without branching on them, the entries that change
               nothing: a node evaluated earlier in this pass as part of a
               group, and simple nodes / nodes with no conducting channel
               (group of one) whose value formula gives their current value.
               Neither has any side effect in netsim (a group of one's stamp
               is never looked at again: n is in this pass's list once). */
            int n, sv, single;
            for (;;) {
                if (i >= nin) goto pass_done;
                n = in[i];
                uint8_t f = c->f[n];
                sv = SV(c, n);
                single = CC_SIMPLE[n] | (c->nxt[CC_HB + n] == 0);
                int marked = !CC_SIMPLE[n] & (c->gmark[n] > pass_start);
                int act = !marked & (!single | (sv != (f & 1)));
                i++;
                if (act) break;
            }
            if (single) {
                STAT(c->st.lone++);
                set_node(c, n, sv);
            } else {
                recalc_group(c, n, ++c->gstamp);
            }
        }
    pass_done:;
    }
    if (c->nout) {
        STAT(c->st.loop_limit_hits++);
        c->nout = 0;
        if (++c->ostamp == 0) { memset(c->omark, 0, sizeof c->omark); c->ostamp = 1; }
    }
}

void ccpu_drive(ccpu_t *c, int node, int level) {
    uint8_t f = (uint8_t)((c->f[node] & 1) | (level ? 2 : 4));
    if (f == c->f[node]) return;
    c->f[node] = f;
    Q(node);
}

void ccpu_release(ccpu_t *c, int node) {
    uint8_t f = (uint8_t)((c->f[node] & 1) | CC_ORIG_PULL[node]);
    if (f == c->f[node]) return;
    c->f[node] = f;
    Q(node);
}

static uint32_t read_bus(const ccpu_t *c, const int16_t *ids, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
        if (ids[i] >= 0 && (c->f[ids[i]] & 1)) v |= 1u << i;
    return v;
}

static void half_clock(ccpu_t *c, int level) {
    ccpu_drive(c, CC_N_CLK0, level);
    ccpu_recalc(c);
}

/* netsim_create + netcpu_init */
void ccpu_init(ccpu_t *c) {
    memset(c, 0, sizeof(*c));
    for (int n = 0; n < CC_NN; n++) {
        c->f[n] = CC_ORIG_PULL[n];
        c->ngnd[n] = CC_GND_INIT[n];
        c->npwr[n] = CC_PWR_INIT[n];
    }
    c->f[CC_VCC] = 1;
    c->f[CC_VSS] = 0;
    for (int k = 0; k < CC_NCH; k++)
        if (CC_CH_INIT_ON[k]) {
            LINK(2 * k + 1, CC_SOTHER1[2 * k + 2]);   /* slot 2k: in c1's list */
            LINK(2 * k + 2, CC_SOTHER1[2 * k + 1]);   /* slot 2k+1: in c2's list */
        }
    c->ostamp = 1;
    c->gstamp = 1;

    ccpu_drive(c, CC_N_RES, 0);
    ccpu_drive(c, CC_N_CLK0, 1);
    ccpu_drive(c, CC_N_RDY, 1);
    ccpu_drive(c, CC_N_SO, 1);
    ccpu_drive(c, CC_N_IRQ, 1);
    ccpu_drive(c, CC_N_NMI, 1);
    for (int i = 0; i < 8; i++) ccpu_drive(c, BUS_DB[i], 0);
    c->db_driven = 0;
    for (int n = 0; n < CC_NN; n++)            /* netsim_stabilize */
        if (n != CC_VSS && n != CC_VCC) Q(n);
    ccpu_recalc(c);
    for (int i = 0; i < 8; i++) { half_clock(c, 0); half_clock(c, 1); }
    c->prev_read = true;
    memset(&c->st, 0, sizeof c->st);
}

void ccpu_free(ccpu_t *c) { (void)c; }

uint64_t ccpu_tick(ccpu_t *c, uint64_t pins) {
    if (c->prev_read) {
        int d = CPU_GET_DATA(pins);
        if (d != c->db_driven) {
            for (int i = 0; i < 8; i++) ccpu_drive(c, BUS_DB[i], (d >> i) & 1);
            c->db_driven = d;
            ccpu_recalc(c);
        }
    }
    half_clock(c, 0);
    ccpu_drive(c, CC_N_RDY, (pins & CPU_RDY) ? 1 : 0);
    ccpu_drive(c, CC_N_IRQ, (pins & CPU_IRQ) ? 0 : 1);
    ccpu_drive(c, CC_N_NMI, (pins & CPU_NMI) ? 0 : 1);
    ccpu_drive(c, CC_N_RES, (pins & CPU_RES) ? 0 : 1);
    ccpu_recalc(c);
    half_clock(c, 1);

    uint64_t out = pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC);
    out |= read_bus(c, BUS_AB, 16);
    int rw = c->f[CC_N_RW] & 1;
    if (rw) out |= CPU_RW;
    if (c->f[CC_N_SYNC] & 1) out |= CPU_SYNC;
    if (!rw) {
        out = CPU_SET_DATA(out, read_bus(c, BUS_DB, 8));
        c->db_driven = -1;
    }
    c->prev_read = rw != 0;
    c->cycles++;
    return out;
}

int ccpu_node_id(const char *name) {
    for (size_t i = 0; i < sizeof CC_NAMES / sizeof CC_NAMES[0]; i++)
        if (strcmp(CC_NAMES[i].name, name) == 0) return CC_NAMES[i].node;
    return -1;
}
/* drive of node n's group in a stable state: 0 floating (value = stored
   charge), 1 pulled up only, 2 driven by a pad, 3 conducting to VSS/VCC */
int ccpu_group_drive(const ccpu_t *c, int n) {
    static uint8_t seen[CC_NN];
    static uint16_t grp[CC_NN];
    memset(seen, 0, sizeof seen);
    int ng = 1, gnd = 0, pwr = 0;
    uint8_t acc = 0;
    grp[0] = (uint16_t)n; seen[n] = 1;
    for (int i = 0; i < ng; i++) {
        int m = grp[i];
        gnd |= c->ngnd[m]; pwr |= c->npwr[m]; acc |= c->f[m];
        for (int sl = c->nxt[CC_HB + m]; sl; sl = c->nxt[sl]) {
            int o = CC_SOTHER1[sl];
            if (!seen[o]) { seen[o] = 1; grp[ng++] = (uint16_t)o; }
        }
    }
    if (gnd || pwr) return 3;
    if (acc & 4) return 2;
    if (acc & 2) {
        for (int i = 0; i < ng; i++) if ((c->f[grp[i]] & 2) && !CC_ORIG_PULL[grp[i]]) return 2;
        return 1;
    }
    return 0;
}

const char *ccpu_node_name(int node) {
    for (size_t i = 0; i < sizeof CC_NAMES / sizeof CC_NAMES[0]; i++)
        if (CC_NAMES[i].node == node) return CC_NAMES[i].name;
    return NULL;
}
uint8_t ccpu_a(const ccpu_t *c) { return (uint8_t)read_bus(c, BUS_A, 8); }
uint8_t ccpu_x(const ccpu_t *c) { return (uint8_t)read_bus(c, BUS_X, 8); }
uint8_t ccpu_y(const ccpu_t *c) { return (uint8_t)read_bus(c, BUS_Y, 8); }
uint8_t ccpu_s(const ccpu_t *c) { return (uint8_t)read_bus(c, BUS_S, 8); }
uint8_t ccpu_ir(const ccpu_t *c) { return (uint8_t)read_bus(c, BUS_IR, 8); }
uint16_t ccpu_pc(const ccpu_t *c) { return (uint16_t)(read_bus(c, BUS_PCL, 8) | (read_bus(c, BUS_PCH, 8) << 8)); }
uint8_t ccpu_p(const ccpu_t *c) { return (uint8_t)((read_bus(c, BUS_P, 8) & 0xCF) | 0x20); }
const ccpu_stats_t *ccpu_stats(const ccpu_t *c) { return &c->st; }

#define CCPU_STATE_MAGIC 0x43435055u   /* "CCPU" */
size_t ccpu_state_size(void) { return 8 + sizeof(ccpu_t); }
void ccpu_state_save(const ccpu_t *c, void *buf) {
    uint32_t hdr[2] = { CCPU_STATE_MAGIC, (uint32_t)sizeof(ccpu_t) };
    memcpy(buf, hdr, 8);
    memcpy((uint8_t *)buf + 8, c, sizeof *c);
}
int ccpu_state_load(ccpu_t *c, const void *buf, size_t len) {
    uint32_t hdr[2];
    if (len != ccpu_state_size()) return -1;
    memcpy(hdr, buf, 8);
    if (hdr[0] != CCPU_STATE_MAGIC || hdr[1] != sizeof(ccpu_t)) return -1;
    memcpy(c, (const uint8_t *)buf + 8, sizeof *c);
    return 0;
}

/* Load the dynamic state of a harness netsim instance (netsim_state_save()
 * layout of harness/src/netsim.c): node values and pulls, rail counts and
 * the order of the conducting-transistor lists.  Used by the lockstep test
 * to resynchronise after a reported mismatch; also lets a netlist save
 * state continue on the compiled core. */
int ccpu_import_netsim(ccpu_t *c, const void *buf, size_t len, int prev_read, int db_driven) {
    const size_t nn = CC_NN, nt = CC_NT;
    const size_t hdr = 4 * 4 + 4 * 8;   /* nn, nt, outstamp, gstamp + netsim_stats_t */
    size_t need = hdr + nn + nn + 2 * nn + 2 * nn + nt + 4 * nn + 4 * 2 * nt + 4 * 2 * nt + 4 * nn + 4 * nn;
    if (len != need) return -1;
    const uint8_t *p = buf;
    uint32_t h[4];
    memcpy(h, p, 16);
    if (h[0] != nn || h[1] != nt) return -1;
    p += hdr;
    const uint8_t *flags = p; p += nn;
    p += nn;                                   /* orig_pull (static) */
    const uint8_t *ngnd = p; p += 2 * nn;
    const uint8_t *npwr = p; p += 2 * nn;
    p += nt;                                   /* ton (= gate values) */
    const uint8_t *head = p; p += 4 * nn;
    const uint8_t *snext = p;
    static int16_t t2k[CC_NT];
    for (size_t t = 0; t < nt; t++) t2k[t] = -1;
    for (int k = 0; k < CC_NCH; k++) t2k[CC_CH_T[k]] = (int16_t)k;

    ccpu_t keep = *c;
    memset(c, 0, sizeof *c);
    c->cycles = keep.cycles;
    c->st = keep.st;
    for (size_t n = 0; n < nn; n++) {
        uint16_t g, w;
        c->f[n] = flags[n];
        memcpy(&g, ngnd + 2 * n, 2); memcpy(&w, npwr + 2 * n, 2);
        if (g > 255 || w > 255) return -1;
        c->ngnd[n] = (uint8_t)g; c->npwr[n] = (uint8_t)w;
        int32_t sl; memcpy(&sl, head + 4 * n, 4);
        int prev = CC_HB + (int)n;
        c->nxt[prev] = 0;
        while (sl >= 0) {
            int t = sl >> 1, b = sl & 1;
            if (t2k[t] < 0) return -1;
            int my = 2 * t2k[t] + b + 1;
            c->prv[my] = (uint16_t)prev;
            c->nxt[my] = 0;
            c->nxt[prev] = (uint16_t)my;
            prev = my;
            memcpy(&sl, snext + 4 * (size_t)sl, 4);
        }
    }
    c->ostamp = 1;
    c->gstamp = 1;
    c->prev_read = prev_read != 0;
    c->db_driven = db_driven;
    return 0;
}
