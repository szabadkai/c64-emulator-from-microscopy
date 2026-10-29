"""Code generator for the levelized core (lcpu): one straight-line C function
per clock phase (plus the cones of the data pads and of RDY/IRQ/NMI/RES).

Every node value is a byte v[n] (0/1).  A node's drive is encoded as a
priority mask, so that the value of a group of connected nodes is the
decode of the OR of its members' masks (netsim's rule):
    bit5 conducting transistor to VSS   -> 0
    bit4 conducting transistor to VCC   -> 1
    bit3 pad driven low                 -> 0
    bit2 pull-up (netlist) / pad high   -> 1
    bit1 stored charge (old value)      -> 1 if any member holds charge
Each step of the generated code is one of netsim's group evaluations
(the value of a connected group from the state at that moment), so the
whole sweep is a legal netsim evaluation order -- just not netsim's own.
"""
import os
from collections import defaultdict
from levelize import Phase
from phases import PADS

DEC = [0] * 64
for m in range(64):
    if m & 32:
        DEC[m] = 0
    elif m & 16:
        DEC[m] = 1
    elif m & 8:
        DEC[m] = 0
    elif m & 4:
        DEC[m] = 1
    else:
        DEC[m] = (m >> 1) & 1


class Gen:
    def __init__(self, N):
        self.N = N
        self.pads = [N.byname[p] for p in PADS]
        self.padidx = {n: i for i, n in enumerate(self.pads)}
        self.tables = []          # generic data for self-gated components
        self.sg_count = 0

    # ---- expressions ---------------------------------------------------
    def or_expr(self, gates):
        return " | ".join("v[%d]" % g for g in gates) if gates else "0"

    def mask_expr(self, P, n):
        """drive mask of node n (bits as in the module doc), C expression"""
        parts = []
        if P.gnd_const[n]:
            return "32"
        if P.gnd[n]:
            parts.append("((%s) << 5)" % self.or_expr(P.gnd[n]))
        if P.pwr_const[n]:
            parts.append("16")
        elif P.pwr[n]:
            parts.append("((%s) << 4)" % self.or_expr(P.pwr[n]))
        if n in self.padidx:
            parts.append("pad[%d]" % self.padidx[n])     # bit3 low / bit2 high
        elif self.N.pullup[n]:
            parts.append("4")
        parts.append("(v[%d] << 1)" % n)
        return " | ".join(parts)

    # ---- one phase component -------------------------------------------
    def emit_pccc(self, P, q, out, track):
        """append C statements evaluating phase component q; if track,
        OR (old ^ new) of every node into 'chg'"""
        N = self.N
        nodes = P.pccc[q]
        if q in P.selfgated and len(P.sources[q]) > 1:
            self.emit_selfgated(P, q, out, track)
            return
        if q in P.selfgated and not getattr(self, "_in_loop", False):
            # one source node: a simultaneous evaluation of all groups is the
            # sequential order "every other group, then the source's group";
            # repeat until nothing changes
            self._in_loop = True
            body = []
            self.emit_pccc(P, q, body, True)
            self._in_loop = False
            out.append("for (int k_ = 0;; k_++) { unsigned chg_o = chg; chg = 0;")
            out.extend("  " + b for b in body)
            out.append("  unsigned c_ = chg; chg = chg_o | c_; if (!c_ || k_ >= LC_LOOP_LIMIT) { if (c_) st->loop_limit_hits++; break; } }")
            return
        if len(nodes) == 1:
            n = nodes[0]
            simple = self.simple_expr(P, n)
            if track:
                out.append("{ uint8_t o_ = v[%d]; v[%d] = %s; chg |= o_ ^ v[%d]; }" % (n, n, simple, n))
            else:
                out.append("v[%d] = %s;" % (n, simple))
            return
        # super-nodes: nodes joined by channels that are on for the whole phase
        sup = {n: n for n in nodes}

        def f(x):
            while sup[x] != x:
                sup[x] = sup[sup[x]]
                x = sup[x]
            return x
        dyn = []
        for g, a, b, st in P.chan:
            if P.cid[a] != q:
                continue
            if st == 1:
                ra, rb = f(a), f(b)
                if ra != rb:
                    sup[ra] = rb
        for g, a, b, st in P.chan:
            if P.cid[a] != q or st == 1:
                continue
            dyn.append((g, a, b))
        groups = defaultdict(list)
        for n in nodes:
            groups[f(n)].append(n)
        sids = sorted(groups)
        sidx = {s: i for i, s in enumerate(sids)}
        # merge parallel dynamic edges between the same super-node pair
        edges = defaultdict(list)
        for g, a, b in dyn:
            ra, rb = sidx[f(a)], sidx[f(b)]
            if ra == rb:
                continue          # always merged anyway
            key = (min(ra, rb), max(ra, rb))
            edges[key].append(g)
        E = sorted(edges.items())
        lines = ["{"]
        for i, s in enumerate(sids):
            lines.append("  unsigned m%d = %s;" % (i, " | ".join("(%s)" % self.mask_expr(P, n) for n in groups[s])))
        if E:
            # forest? (spanning-tree check on super-nodes)
            par = list(range(len(sids)))

            def ff(x):
                while par[x] != x:
                    par[x] = par[par[x]]
                    x = par[x]
                return x
            forest = True
            for (a, b), gs in E:
                ra, rb = ff(a), ff(b)
                if ra == rb:
                    forest = False
                    break
                par[ra] = rb
            for k, ((a, b), gs) in enumerate(E):
                lines.append("  unsigned e%d = %s;" % (k, self.or_expr(gs)))
            if forest:
                # up pass (leaves first) then down pass over a rooted forest
                adj = defaultdict(list)
                for k, ((a, b), gs) in enumerate(E):
                    adj[a].append((b, k))
                    adj[b].append((a, k))
                seen, orderE = set(), []
                for r in range(len(sids)):
                    if r in seen:
                        continue
                    seen.add(r)
                    stack = [r]
                    bfs = []
                    while stack:
                        x = stack.pop(0)
                        for y, k in sorted(adj[x]):
                            if y not in seen:
                                seen.add(y)
                                bfs.append((x, y, k))   # parent x, child y
                                stack.append(y)
                    orderE += bfs
                for x, y, k in reversed(orderE):      # child -> parent
                    lines.append("  m%d |= e%d ? m%d : 0;" % (x, k, y))
                for x, y, k in orderE:                # parent -> child
                    lines.append("  m%d = e%d ? m%d : m%d;" % (y, k, x, y))
            else:
                lines.append("  for (;;) { unsigned d_ = 0, t_;")
                for k, ((a, b), gs) in enumerate(E):
                    lines.append("    t_ = e%d ? (m%d | m%d) : 0; d_ |= (t_ & ~m%d) | (t_ & ~m%d); m%d |= t_; m%d |= t_;"
                                 % (k, a, b, a, b, a, b))
                lines.append("    if (!d_) break; }")
        for i, s in enumerate(sids):
            for n in groups[s]:
                if track:
                    lines.append("  chg |= v[%d] ^ DEC[m%d]; v[%d] = DEC[m%d];" % (n, i, n, i))
                else:
                    lines.append("  v[%d] = DEC[m%d];" % (n, i))
        lines.append("}")
        out.extend(lines)

    def simple_expr(self, P, n):
        """value of a lone node, simplified where the structure allows"""
        if P.gnd_const[n]:
            return "0"
        g = self.or_expr(P.gnd[n])
        if n in self.padidx or P.pwr[n] or P.pwr_const[n]:
            return "DEC[%s]" % self.mask_expr(P, n)
        if self.N.pullup[n]:
            return "(uint8_t)!(%s)" % g if P.gnd[n] else "1"
        if not P.gnd[n]:
            return "v[%d]" % n
        return "(uint8_t)(!(%s) & v[%d])" % (g, n)

    def emit_selfgated(self, P, q, out, track):
        """components whose nodes gate their own transistors: netsim's group
        evaluation, one group at a time, until nothing changes"""
        N = self.N
        nodes = P.pccc[q]
        loc = {n: i for i, n in enumerate(nodes)}
        # per node: rail gates (dynamic), constant drives, static pull, pad
        rows = []
        srcs = P.sources[q]
        for n in nodes:
            rows.append((n, P.gnd[n], P.pwr[n], 1 if P.gnd_const[n] else 0, 1 if (P.pwr_const[n]) else 0,
                         self.padidx.get(n, -1), self.N.pullup[n], 1 if n in srcs else 0))
        chans = []
        for g, a, b, st in P.chan:
            if P.cid[a] == q:
                chans.append((g if st is None else -1, loc[a], loc[b]))
        tid = self.sg_count
        self.sg_count += 1
        self.tables.append((tid, rows, chans, len(nodes)))
        out.append("chg |= sg_eval(v, pad, &SG%d, st);" % tid if track else "(void)sg_eval(v, pad, &SG%d, st);" % tid)

    # ---- whole phase / cone ----------------------------------------------
    def emit_sweep(self, P, name, subset=None, prologue=True):
        out = []
        out.append("static void %s(uint8_t *restrict v, const uint8_t *restrict pad, lcpu_stats_t *st) {" % name)
        out.append("    unsigned chg; (void)chg; int it_; (void)it_;")
        if prologue:
            for n in P.det_order:
                out.append("    v[%d] = %d;" % (n, P.det[n]))
        for scc in P.order:
            members = [q for q in scc if subset is None or q in subset]
            if not members:
                continue
            body = []
            if len(scc) == 1 and scc[0] not in P.selfgated:
                self.emit_pccc(P, scc[0], body, False)
                out.extend("    " + b for b in body)
            elif len(scc) == 1:
                body.append("chg = 0;")
                self.emit_pccc(P, scc[0], body, True)
                out.extend("    " + b for b in body)
            else:
                out.append("    for (it_ = 0;; it_++) { chg = 0;")
                for q in members:
                    b2 = []
                    self.emit_pccc(P, q, b2, True)
                    out.extend("      " + b for b in b2)
                out.append("      if (!chg) break;")
                out.append("      if (it_ >= LC_LOOP_LIMIT) { st->loop_limit_hits++; break; } }")
                out.append("    st->scc_iters += (unsigned)it_;")
        out.append("}")
        return out

    def emit_tables(self):
        out = []
        for tid, rows, chans, nn in self.tables:
            out.append("static const lc_sg_node_t SG%d_N[%d] = {" % (tid, len(rows)))
            for n, gnd, pwr, gc, pc, padi, pu, src in rows:
                out.append("  {%d,%d,%d,%d,%d,%d,%d,%d,{%s},{%s}}," % (
                    n, gc, pc, padi, pu, src, len(gnd), len(pwr),
                    ",".join(str(x) for x in gnd) or "0", ",".join(str(x) for x in pwr) or "0"))
            out.append("};")
            out.append("static const lc_sg_chan_t SG%d_C[%d] = {" % (tid, len(chans)))
            for g, a, b in chans:
                out.append("  {%d,%d,%d}," % (g, a, b))
            out.append("};")
            out.append("static const lc_sg_t SG%d = { %d, %d, SG%d_N, SG%d_C };" % (tid, len(rows), len(chans), tid, tid))
        return out


def emit(N, outdir):
    g = Gen(N)
    P1, P2 = Phase(N, 0), Phase(N, 1)
    ctl = [N.byname[x] for x in ("rdy", "irq", "nmi", "res")]
    dbp = [N.byname["db%d" % i] for i in range(8)]
    body = []
    body += g.emit_sweep(P1, "lc_sweep_phi1")
    body += g.emit_sweep(P2, "lc_sweep_phi2")
    body += g.emit_sweep(P1, "lc_cone_ctl", subset=P1.cone(ctl), prologue=False)
    body += g.emit_sweep(P2, "lc_cone_db", subset=P2.cone(dbp), prologue=False)
    maxrail = 0
    for tid, rows, chans, nn in g.tables:
        for r in rows:
            maxrail = max(maxrail, len(r[1]), len(r[2]))
    maxsg = max([nn for tid, rows, chans, nn in g.tables] + [1])
    head = ["/* GENERATED by ncc.py (tools/lgen.py) - do not edit */",
            "#define LC_NPADS %d" % len(g.pads),
            "#define LC_MAXRAIL %d" % max(maxrail, 1),
            "#define LC_MAXSG %d" % maxsg,
            "static const int16_t LC_PADS[LC_NPADS] = {%s};" % ",".join(str(p) for p in g.pads),
            "static const uint8_t DEC[64] = {%s};" % ",".join(str(x) for x in DEC)]
    head.insert(0, "#pragma once")
    head.insert(5, "#define LC_COMP_PHI1 %d   /* phase components per sweep / cone */\n#define LC_COMP_PHI2 %d\n#define LC_COMP_CTL %d\n#define LC_COMP_DB %d"
                % (len(P1.pccc), len(P2.pccc), len(P1.cone(ctl)), len(P2.cone(dbp))))
    tabs = g.emit_tables()
    with open(os.path.join(outdir, "lcpu_net.h"), "w") as f:
        f.write("\n".join(head[:6]) + "\n")
    with open(os.path.join(outdir, "lcpu_net.inc"), "w") as f:
        f.write("\n".join(head[6:]) + "\n")
        f.write("\n".join(tabs) + "\n")
        f.write("\n".join(body) + "\n")
    stats = {
        "phi1": (len(P1.det), len(P1.pccc), [len(s) for s in P1.order if len(s) > 1], len(P1.selfgated)),
        "phi2": (len(P2.det), len(P2.pccc), [len(s) for s in P2.order if len(s) > 1], len(P2.selfgated)),
        "cone_ctl": len(P1.cone(ctl)), "cone_db": len(P2.cone(dbp)),
    }
    return stats
