/* netsim.c - switch-level NMOS netlist simulator (see netsim.h).
 *
 * Data layout ("compressed sparse row" lists per node):
 *   chan   : (transistor, other-node) pairs for transistors whose channel
 *            joins this node to another ordinary node,
 *   gchan  : channel transistors gated by this node,
 *   ggnd   : nodes pulled to VSS by a transistor gated by this node,
 *   gpwr   : nodes pulled to VCC by a transistor gated by this node,
 *   dep    : channel ends to re-evaluate when this node goes low (when it
 *            goes high, one end per transistor is enough: the transistor now
 *            merges both ends into one group).
 * Rail connections are not walked: every node keeps a count of conducting
 * transistors to VSS (ngnd) and to VCC (npwr), updated when gates switch.
 * Channel transistors that are currently ON are kept in a doubly linked list
 * per node ("slots": 2 per transistor, one for each channel end), so a group
 * walk only visits conducting transistors - the big internal buses have dozens
 * of pass transistors of which only a few conduct at any time.
 *
 * A node without channel connections ("simple" node: the output of a NOR
 * gate, inverter or pad) is always a group of its own; its value is
 *     ngnd ? 0 : npwr ? 1 : external pull : netlist pull-up : old charge
 * When a gate switch changes its counts we only queue it if that value
 * would actually change.
 *
 * Two prunings skip evaluations that provably cannot change anything
 * (disable with -DNETSIM_NO_PRUNE to cross-check traces):
 *   - a transistor turning ON between two nodes that already have the same
 *     value: merging two groups of equal value yields that value again
 *     (VSS > VCC > pull-down > pull-up > charge is monotone under union);
 *   - a rail count changing between non-zero values (1 <-> 2 ...): the
 *     group was and stays connected to that rail.
 * Invariant: every node either holds its group's value or some member of
 * its group is queued, so skipped evaluations would have been no-ops.
 *
 * Group membership and list de-duplication use generation stamps.  A node
 * already evaluated as part of a group earlier in the same pass is skipped:
 * any later change that could affect it re-queues one of its group members
 * for the next pass. */
#include "netsim.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_VAL = 1, F_PULLUP = 2, F_PULLDOWN = 4 };

#define LOOP_LIMIT 1000

typedef struct {
    uint32_t chan, gchan, ggnd, gpwr, dep;   /* start offsets; end = next node's */
} noff_t;

struct netsim {
    int nn, nt, vss, vcc;
    uint8_t *flags;      /* per node: F_VAL | F_PULLUP | F_PULLDOWN */
    uint8_t *orig_pull;  /* per node: netlist pull-up (F_PULLUP) */
    uint8_t *simple;     /* per node: no channel connections */
    uint16_t *ngnd, *npwr;
    uint8_t *ton;        /* per channel transistor: conducting */
    /* live adjacency of conducting channel transistors */
    int32_t *head;       /* per node: first slot, -1 = none */
    int32_t *snext, *sprev;
    uint16_t *sother;    /* slot 2t: in c1's list, other end c2; slot 2t+1: in c2's list */

    noff_t *off;         /* nn + 1 entries */
    uint32_t *chan;      /* (t << 16) | other */
    uint16_t *gchan, *ggnd, *gpwr, *dep;

    uint16_t *list_a, *list_b, *listin, *listout;
    int nin, nout;
    uint32_t *outmark; uint32_t outstamp;
    uint32_t *gmark;   uint32_t gstamp;
    uint16_t *group;

    netsim_stats_t st;
};

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz);
    if (!p) { fprintf(stderr, "netsim: out of memory\n"); exit(1); }
    return p;
}

typedef struct { uint32_t key, val; } pair_t;
static int cmp_pair(const void *a, const void *b) {
    const pair_t *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->val < y->val ? -1 : (x->val > y->val);
}

/* sort pairs by node, optionally drop duplicates, write values and per-node
   start offsets (field at byte offset 'field' of noff_t) */
static void *build_list(netsim_t *s, pair_t *p, int np, int uniq, size_t field, int wide) {
    qsort(p, np, sizeof(pair_t), cmp_pair);
    void *vals = xcalloc(np, wide ? 4 : 2);
    uint32_t *count = xcalloc(s->nn + 1, 4);
    int w = 0;
    for (int i = 0; i < np; i++) {
        if (uniq && i > 0 && p[i].key == p[i - 1].key && p[i].val == p[i - 1].val) continue;
        if (wide) ((uint32_t *)vals)[w] = p[i].val; else ((uint16_t *)vals)[w] = (uint16_t)p[i].val;
        count[p[i].key]++;
        w++;
    }
    uint32_t acc = 0;
    for (int n = 0; n <= s->nn; n++) {
        *(uint32_t *)((char *)&s->off[n] + field) = acc;
        if (n < s->nn) acc += count[n];
    }
    free(count);
    return vals;
}

static inline void link_slot(netsim_t *s, int sl) {
    int node = s->sother[sl ^ 1];
    int h = s->head[node];
    s->snext[sl] = h;
    s->sprev[sl] = -1;
    if (h >= 0) s->sprev[h] = sl;
    s->head[node] = sl;
}
static inline void unlink_slot(netsim_t *s, int sl) {
    int p = s->sprev[sl], n = s->snext[sl];
    if (p >= 0) s->snext[p] = n; else s->head[s->sother[sl ^ 1]] = n;
    if (n >= 0) s->sprev[n] = p;
}
static inline void link_trans(netsim_t *s, int t) { link_slot(s, 2 * t); link_slot(s, 2 * t + 1); }
static inline void unlink_trans(netsim_t *s, int t) { unlink_slot(s, 2 * t); unlink_slot(s, 2 * t + 1); }

netsim_t *netsim_create(int nnodes, int ntrans, const uint16_t (*trans)[3],
                        const uint16_t *pullups, int npullups, int vss, int vcc) {
    netsim_t *s = xcalloc(1, sizeof(*s));
    s->nn = nnodes; s->vss = vss; s->vcc = vcc;
    s->flags = xcalloc(nnodes, 1);
    s->orig_pull = xcalloc(nnodes, 1);
    s->simple = xcalloc(nnodes, 1);
    s->ngnd = xcalloc(nnodes, 2);
    s->npwr = xcalloc(nnodes, 2);
    for (int i = 0; i < npullups; i++) s->orig_pull[pullups[i]] = F_PULLUP;
    s->orig_pull[vcc] = 0; s->orig_pull[vss] = 0;
    for (int i = 0; i < nnodes; i++) s->flags[i] = s->orig_pull[i];
    s->flags[vcc] = F_VAL; s->flags[vss] = 0;

    /* de-duplicate transistors (same gate, same unordered channel), drop
       no-op channels, put a rail (if any) on c2 */
    uint16_t (*tt)[3] = xcalloc(ntrans, sizeof(*tt));
    int nt = 0;
    for (int i = 0; i < ntrans; i++) {
        int g = trans[i][0], c1 = trans[i][1], c2 = trans[i][2];
        if (c1 == c2) continue;
        if ((c1 == vss || c1 == vcc) && (c2 == vss || c2 == vcc)) continue;
        if (c1 == vss || c1 == vcc) { int t = c1; c1 = c2; c2 = t; }
        int dup = 0;
        for (int j = 0; j < nt && !dup; j++)
            if (tt[j][0] == g && ((tt[j][1] == c1 && tt[j][2] == c2) || (tt[j][1] == c2 && tt[j][2] == c1)))
                dup = 1;
        if (dup) continue;
        tt[nt][0] = (uint16_t)g; tt[nt][1] = (uint16_t)c1; tt[nt][2] = (uint16_t)c2; nt++;
    }
    s->nt = nt;
    s->ton = xcalloc(nt, 1);
    s->off = xcalloc(nnodes + 1, sizeof(noff_t));
    s->head = xcalloc(nnodes, 4);
    for (int i = 0; i < nnodes; i++) s->head[i] = -1;
    s->snext = xcalloc(2 * nt, 4);
    s->sprev = xcalloc(2 * nt, 4);
    s->sother = xcalloc(2 * nt, 2);

    pair_t *pc = xcalloc(2 * nt, sizeof(pair_t)); int npc = 0;
    pair_t *pgc = xcalloc(nt, sizeof(pair_t)); int npgc = 0;
    pair_t *pgg = xcalloc(nt, sizeof(pair_t)); int npgg = 0;
    pair_t *pgp = xcalloc(nt, sizeof(pair_t)); int npgp = 0;
    pair_t *pd = xcalloc(2 * nt, sizeof(pair_t)); int npd = 0;
    for (int t = 0; t < nt; t++) {
        int g = tt[t][0], c1 = tt[t][1], c2 = tt[t][2];
        int gate_on = (s->flags[g] & F_VAL) != 0;   /* only VCC is high at power-up */
        int gate_rail = (g == vss || g == vcc);
        if (c2 == vss) {
            if (gate_on) s->ngnd[c1]++;
            if (!gate_rail) pgg[npgg++] = (pair_t){ g, c1 };
        } else if (c2 == vcc) {
            if (gate_on) s->npwr[c1]++;
            if (!gate_rail) pgp[npgp++] = (pair_t){ g, c1 };
        } else {
            pc[npc++] = (pair_t){ c1, ((uint32_t)t << 16) | c2 };
            pc[npc++] = (pair_t){ c2, ((uint32_t)t << 16) | c1 };
            s->ton[t] = (uint8_t)gate_on;
            s->sother[2 * t] = (uint16_t)c2;
            s->sother[2 * t + 1] = (uint16_t)c1;
            if (!gate_rail) {
                pgc[npgc++] = (pair_t){ g, t };
                pd[npd++] = (pair_t){ g, c1 };
                pd[npd++] = (pair_t){ g, c2 };
            }
        }
    }
    s->chan  = build_list(s, pc, npc, 0, offsetof(noff_t, chan), 1);
    s->gchan = build_list(s, pgc, npgc, 0, offsetof(noff_t, gchan), 0);
    s->ggnd  = build_list(s, pgg, npgg, 0, offsetof(noff_t, ggnd), 0);
    s->gpwr  = build_list(s, pgp, npgp, 0, offsetof(noff_t, gpwr), 0);
    s->dep   = build_list(s, pd, npd, 1, offsetof(noff_t, dep), 0);
    free(pc); free(pgc); free(pgg); free(pgp); free(pd); free(tt);
    for (int n = 0; n < nnodes; n++) s->simple[n] = s->off[n].chan == s->off[n + 1].chan;
    for (int t = 0; t < nt; t++)
        if (s->ton[t]) link_trans(s, t);

    s->list_a = xcalloc(nnodes, 2); s->list_b = xcalloc(nnodes, 2);
    s->listin = s->list_a; s->listout = s->list_b;
    s->outmark = xcalloc(nnodes, 4); s->outstamp = 1;
    s->gmark = xcalloc(nnodes, 4); s->gstamp = 1;
    s->group = xcalloc(nnodes, 2);
    return s;
}

void netsim_destroy(netsim_t *s) {
    if (!s) return;
    free(s->flags); free(s->orig_pull); free(s->simple); free(s->ngnd); free(s->npwr);
    free(s->ton); free(s->off); free(s->head); free(s->snext); free(s->sprev); free(s->sother); free(s->chan); free(s->gchan); free(s->ggnd); free(s->gpwr);
    free(s->dep);
    free(s->list_a); free(s->list_b); free(s->outmark); free(s->gmark); free(s->group);
    free(s);
}

static inline void queue(netsim_t *s, int n) {
    if (s->outmark[n] != s->outstamp) {
        s->outmark[n] = s->outstamp;
        s->listout[s->nout++] = (uint16_t)n;
    }
}

/* value of a simple node from its rail counts and pulls */
static inline int simple_value(const netsim_t *s, int n) {
    if (s->ngnd[n]) return 0;
    if (s->npwr[n]) return 1;
    uint8_t f = s->flags[n];
    if (f & F_PULLDOWN) return 0;
    if (f & F_PULLUP) return 1;
    return f & F_VAL;
}

/* a rail count of d changed; 'edge' = it crossed zero.  Queue d if its
   value may change (simple nodes are checked right away). */
static inline void consider(netsim_t *s, int d, int edge) {
#ifdef NETSIM_NO_PRUNE
    (void)edge;
    queue(s, d);
#else
    if (s->simple[d]) {
        if (simple_value(s, d) != (s->flags[d] & F_VAL)) queue(s, d);
    } else if (edge) {
        queue(s, d);
    }
#endif
}

void netsim_drive(netsim_t *s, int node, int level) {
    uint8_t f = (s->flags[node] & F_VAL) | (level ? F_PULLUP : F_PULLDOWN);
    if (f == s->flags[node]) return;
    s->flags[node] = f;
    queue(s, node);
}

void netsim_release(netsim_t *s, int node) {
    uint8_t f = (s->flags[node] & F_VAL) | s->orig_pull[node];
    if (f == s->flags[node]) return;
    s->flags[node] = f;
    queue(s, node);
}

int netsim_get(const netsim_t *s, int node) { return s->flags[node] & F_VAL; }
int netsim_num_transistors(const netsim_t *s) { return s->nt; }
const netsim_stats_t *netsim_stats(const netsim_t *s) { return &s->st; }

/* node m changed to v: switch the transistors it gates, queue dependants */
static inline void set_node(netsim_t *s, int m, int v) {
    s->flags[m] ^= F_VAL;
    const noff_t *o = &s->off[m], *e = o + 1;
    if (v) {
        for (uint32_t k = o->gchan; k < e->gchan; k++) {
            int t = s->gchan[k];
            s->ton[t] = 1;
            link_trans(s, t);
#ifdef NETSIM_NO_PRUNE
            queue(s, s->sother[2 * t + 1]);
#else
            int c1 = s->sother[2 * t + 1], c2 = s->sother[2 * t];
            if ((s->flags[c1] ^ s->flags[c2]) & F_VAL) queue(s, c1);
#endif
        }
        for (uint32_t k = o->ggnd; k < e->ggnd; k++) { int d = s->ggnd[k]; consider(s, d, s->ngnd[d]++ == 0); }
        for (uint32_t k = o->gpwr; k < e->gpwr; k++) { int d = s->gpwr[k]; consider(s, d, s->npwr[d]++ == 0); }
    } else {
        for (uint32_t k = o->gchan; k < e->gchan; k++) { int t = s->gchan[k]; s->ton[t] = 0; unlink_trans(s, t); }
        for (uint32_t k = o->ggnd; k < e->ggnd; k++) { int d = s->ggnd[k]; consider(s, d, --s->ngnd[d] == 0); }
        for (uint32_t k = o->gpwr; k < e->gpwr; k++) { int d = s->gpwr[k]; consider(s, d, --s->npwr[d] == 0); }
        for (uint32_t k = o->dep; k < e->dep; k++) queue(s, s->dep[k]);
    }
}

/* evaluate the group containing node n */
static inline void recalc_group(netsim_t *s, int n, uint32_t gs) {
    uint8_t *restrict flags = s->flags;
    const int32_t *restrict snext = s->snext;
    const uint16_t *restrict sother = s->sother;
    uint32_t *restrict gmark = s->gmark;
    uint16_t *restrict group = s->group;
    int ng = 1;
    unsigned gnd = s->ngnd[n], pwr = s->npwr[n];
    uint8_t acc = flags[n];
    group[0] = (uint16_t)n;
    gmark[n] = gs;
    for (int i = 0; i < ng; i++) {
        int m = group[i];
        for (int sl = s->head[m]; sl >= 0; sl = snext[sl]) {
            int o = sother[sl];
            if (gmark[o] == gs) continue;
            gmark[o] = gs;
            group[ng++] = (uint16_t)o;
            acc |= flags[o];
            gnd |= s->ngnd[o];
            pwr |= s->npwr[o];
        }
    }
    int v;
    if (gnd) v = 0;
    else if (pwr) v = 1;
    else if (acc & F_PULLDOWN) v = 0;
    else if (acc & F_PULLUP) v = 1;
    else v = acc & F_VAL;

    s->st.groups++;
    s->st.group_nodes += ng;
    for (int i = 0; i < ng; i++) {
        int m = group[i];
        if ((flags[m] & F_VAL) != v) set_node(s, m, v);
    }
}

void netsim_recalc(netsim_t *s) {
    if (s->gstamp > 0xF0000000u) { memset(s->gmark, 0, s->nn * 4); s->gstamp = 1; }
    int pass;
    for (pass = 0; pass < LOOP_LIMIT && s->nout; pass++) {
        uint16_t *tmp = s->listin; s->listin = s->listout; s->listout = tmp;
        s->nin = s->nout; s->nout = 0;
        if (++s->outstamp == 0) { memset(s->outmark, 0, s->nn * 4); s->outstamp = 1; }
        uint32_t pass_start = s->gstamp;
        s->st.passes++;
        for (int i = 0; i < s->nin; i++) {
            int n = s->listin[i];
            if (s->simple[n]) {
                s->st.groups++;
                s->st.group_nodes++;
                int v = simple_value(s, n);
                if (v != (s->flags[n] & F_VAL)) set_node(s, n, v);
                continue;
            }
            if (s->gmark[n] > pass_start) continue;   /* already evaluated this pass */
            recalc_group(s, n, ++s->gstamp);
        }
    }
    if (s->nout) {
        s->st.loop_limit_hits++;
        s->nout = 0;
        if (++s->outstamp == 0) { memset(s->outmark, 0, s->nn * 4); s->outstamp = 1; }
    }
}

void netsim_stabilize(netsim_t *s) {
    for (int n = 0; n < s->nn; n++)
        if (n != s->vss && n != s->vcc) queue(s, n);
    netsim_recalc(s);
}

/* ------------------------------------------------------------ savestate */
#define NS_FIELDS(X) \
    X(flags, s->nn, 1) X(orig_pull, s->nn, 1) X(ngnd, s->nn, 2) X(npwr, s->nn, 2) \
    X(ton, s->nt, 1) X(head, s->nn, 4) X(snext, 2 * s->nt, 4) X(sprev, 2 * s->nt, 4) \
    X(outmark, s->nn, 4) X(gmark, s->nn, 4)

size_t netsim_state_size(const netsim_t *s) {
    size_t n = 4 * sizeof(uint32_t) + sizeof(netsim_stats_t);
#define SZ(f, cnt, w) n += (size_t)(cnt) * (w);
    NS_FIELDS(SZ)
#undef SZ
    return n;
}

void netsim_state_save(const netsim_t *s, void *buf) {
    uint8_t *p = buf;
    uint32_t hdr[4] = { (uint32_t)s->nn, (uint32_t)s->nt, s->outstamp, s->gstamp };
    memcpy(p, hdr, sizeof hdr); p += sizeof hdr;
    memcpy(p, &s->st, sizeof s->st); p += sizeof s->st;
#define SV(f, cnt, w) memcpy(p, s->f, (size_t)(cnt) * (w)); p += (size_t)(cnt) * (w);
    NS_FIELDS(SV)
#undef SV
}

int netsim_state_load(netsim_t *s, const void *buf, size_t len) {
    if (len != netsim_state_size(s)) return -1;
    const uint8_t *p = buf;
    uint32_t hdr[4];
    memcpy(hdr, p, sizeof hdr); p += sizeof hdr;
    if (hdr[0] != (uint32_t)s->nn || hdr[1] != (uint32_t)s->nt) return -1;
    s->outstamp = hdr[2];
    s->gstamp = hdr[3];
    memcpy(&s->st, p, sizeof s->st); p += sizeof s->st;
#define LD(f, cnt, w) memcpy(s->f, p, (size_t)(cnt) * (w)); p += (size_t)(cnt) * (w);
    NS_FIELDS(LD)
#undef LD
    s->nin = s->nout = 0;
    return 0;
}
