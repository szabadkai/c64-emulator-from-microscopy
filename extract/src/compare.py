"""Stage 3: netlist comparison (extracted vs. visual6502 ground truth).

Two node matchings are computed and BOTH are scored the same way:

  * anchored  -- the honest one. Starts from named nodes only (vcc, vss and the
                 bond-pad names: clk0, rdy, irq, db0.., ab0..). Seeds further node
                 pairs whose anchor-seeded Weisfeiler-Lehman colour (k >= 2 hops of
                 context) is unique and identical in both netlists (netgen-style),
                 then propagates a node matching through the transistor graph:
                 an extracted device whose already-matched nodes pick out a unique
                 consistent GT device fixes the remaining nodes of that device;
                 ties are broken by local structural signatures (WL colour
                 refinement), then (only when nothing else moves) by taking the best
                 candidate. No geometry and no GT node ids are used.
  * oracle    -- diagnostic upper bound: each extracted node is mapped to the GT node
                 that owns most of its pixels (needs GT label rasters; computed by
                 roundtrip.py). Separates "extraction errors" from "matching errors".

Score: a GT transistor (gate, {c1, c2}) counts as recovered when an extracted
transistor maps onto exactly that triple (multiset matching, so parallel devices
count individually). Also reported: device-count, pull-up agreement, nodes mapped.

CLI:
    python -I src/compare.py --ext out/roundtrip/s0.5/netlist.json [--oracle out/roundtrip/s0.5/oracle_map.json]
                             [--report out/roundtrip/s0.5/compare.json]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

ANCHOR_NAMES = (["vcc", "vss", "res", "rw", "sync", "so", "clk0", "clk1out", "clk2out", "rdy", "nmi", "irq"]
                + [f"db{i}" for i in range(8)] + [f"ab{i}" for i in range(16)])


class Netlist:
    def __init__(self, devices, pullups, names):
        # devices: list of (gate, c1, c2)
        self.dev = [(int(g), int(a), int(b)) for g, a, b in devices]
        self.pullups = set(int(p) for p in pullups)
        self.names = {k: int(v) for k, v in names.items()}
        self.by_gate = defaultdict(list)
        self.by_term = defaultdict(list)
        self.nodes = set()
        for i, (g, a, b) in enumerate(self.dev):
            self.by_gate[g].append(i)
            self.by_term[a].append(i)
            if b != a:
                self.by_term[b].append(i)
            self.nodes.update((g, a, b))
        self.nodes |= self.pullups
        self.sig = self._wl(3)

    def _wl(self, iters):
        """Weisfeiler-Lehman style colours (no anchors) used only as tie-breakers."""
        col = {n: hash(("n", n in self.pullups, len(self.by_gate.get(n, ())),
                        len(self.by_term.get(n, ())))) for n in self.nodes}
        hist = [col]
        for _ in range(iters):
            new = {}
            for n in self.nodes:
                ms = []
                for i in self.by_gate.get(n, ()):
                    g, a, b = self.dev[i]
                    ms.append(("g", tuple(sorted((col[a], col[b])))))
                for i in self.by_term.get(n, ()):
                    g, a, b = self.dev[i]
                    other = b if a == n else a
                    ms.append(("t", col[g], col[other]))
                new[n] = hash((col[n], tuple(sorted(ms))))
            col = new
            hist.append(col)
        return hist

    def wl_anchored(self, anchor_names, iters=8, hub_degree=150):
        """WL colours where named anchor nodes start with their own unique colour.
        Anchors and hubs (vcc, vss, any node with > hub_degree device connections)
        keep a frozen colour, so local extraction errors do not leak through the
        power nets into every colour of the chip."""
        inv = {}
        for nm in anchor_names:
            if nm in self.names:
                inv.setdefault(self.names[nm], nm)
        frozen = set(inv) | {n for n in self.nodes
                             if len(self.by_gate.get(n, ())) + len(self.by_term.get(n, ())) > hub_degree}
        col = {n: hash(("a", inv[n]) if n in inv else
                       ("n", n in self.pullups, len(self.by_gate.get(n, ())), len(self.by_term.get(n, ()))))
               for n in self.nodes}
        hist = [col]
        for _ in range(iters):
            new = {}
            for n in self.nodes:
                ms = []
                for i in self.by_gate.get(n, ()):
                    g, a, b = self.dev[i]
                    ms.append(("g", tuple(sorted((col[a], col[b])))))
                for i in self.by_term.get(n, ()):
                    g, a, b = self.dev[i]
                    other = b if a == n else a
                    ms.append(("t", col[g], col[other]))
                new[n] = col[n] if n in frozen else hash((col[n], tuple(sorted(ms))))
            col = new
            hist.append(col)
        return hist

    def sim(self, n, other, m):
        """number of WL levels on which node n (self) and m (other) agree."""
        s = 0
        for h1, h2 in zip(self.sig, other.sig):
            if h1.get(n) == h2.get(m):
                s += 1
            else:
                break
        return s


def load_gt():
    import gt6502
    chip = gt6502.load()
    devs = [(t.gate, t.c1, t.c2) for t in chip.trans]
    names = {k: v for k, v in chip.names.items()}
    return Netlist(devs, chip.pullups(), names), chip


def load_ext(path):
    with open(path) as f:
        d = json.load(f)
    devs = [(t["gate"], t["c1"], t["c2"]) for t in d["transistors"]]
    pull = [n["id"] for n in d["nodes"] if n["pullup"]]
    names = {}
    for n in d["nodes"]:
        for nm in n["names"]:
            names[nm] = n["id"]
    return Netlist(devs, pull, names), d


def match_anchored(ext: Netlist, gt: Netlist, anchors=ANCHOR_NAMES, log=print, hub_degree=150,
                   wl_seed=True, wl_iters=8):
    """Anchored, event-driven node matching (see module doc)."""
    from collections import deque
    M, Minv = {}, {}
    for nm in anchors:
        if nm in ext.names and nm in gt.names:
            e, g = ext.names[nm], gt.names[nm]
            if e in M or g in Minv:
                log(f"  anchor {nm}: conflict (ext node {e} already mapped to another anchor) -- skipped")
                continue
            M[e], Minv[g] = g, e
    n_anchor = len(M)
    # WL seeding (netgen-style): nodes whose anchor-seeded colour is unique in both
    # netlists and identical are matched outright, deepest (most context) level first.
    n_wl = 0
    if wl_seed:
        he, hg = ext.wl_anchored(anchors, wl_iters), gt.wl_anchored(anchors, wl_iters)
        for k in range(wl_iters, 1, -1):
            ce, cg = Counter(he[k].values()), Counter(hg[k].values())
            gt_by_col = {c: n for n, c in hg[k].items() if cg[c] == 1}
            for n, c in he[k].items():
                if n in M or ce[c] != 1 or c not in gt_by_col:
                    continue
                g = gt_by_col[c]
                if g in Minv:
                    continue
                M[n], Minv[g] = g, n
                n_wl += 1
    deg_e = {n: len(ext.by_gate.get(n, ())) + len(ext.by_term.get(n, ())) for n in ext.nodes}
    hubs = {n for n, d in deg_e.items() if d > hub_degree} | {ext.names.get("vcc"), ext.names.get("vss")}
    used_g, done_e = set(), set()

    def candidates(i, allow_hub_only=False):
        g, a, b = ext.dev[i]
        opts = []
        if g in M and (g not in hubs or allow_hub_only):
            opts.append(gt.by_gate.get(M[g], []))
        for x in (a, b):
            if x in M and (x not in hubs or allow_hub_only):
                opts.append(gt.by_term.get(M[x], []))
        if not opts:
            return None
        out = {}
        for j in min(opts, key=len):
            if j in used_g:
                continue
            G, A, B = gt.dev[j]
            if (g in M and M[g] != G) or (g not in M and G in Minv):
                continue
            for X, Y in ((A, B), (B, A)):
                ok = True
                for en, gn in ((a, X), (b, Y)):
                    if (en in M and M[en] != gn) or (en not in M and gn in Minv):
                        ok = False
                        break
                if ok and ((g == a) == (G == X)) and ((g == b) == (G == Y)) and ((a == b) == (X == Y)):
                    out.setdefault((G, X, Y), j)
        return out

    def score(i, key):
        g, a, b = ext.dev[i]
        return sum(ext.sim(en, gt, gn) for en, gn in zip((g, a, b), key) if en not in M)

    def commit(i, key, j):
        g, a, b = ext.dev[i]
        newly = []
        for en, gn in zip((g, a, b), key):
            if en not in M and gn not in Minv:
                M[en], Minv[gn] = gn, en
                newly.append(en)
        used_g.add(j)
        done_e.add(i)
        return newly

    def push(queue, nodes):
        for n in nodes:
            if n in hubs:
                continue
            for k in ext.by_gate.get(n, []) + ext.by_term.get(n, []):
                if k not in done_e:
                    queue.append(k)

    queue = deque()
    push(queue, list(M))
    ambiguous = set()
    forced = 0
    while True:
        while queue:
            i = queue.popleft()
            if i in done_e:
                continue
            cs = candidates(i)
            if cs is None:
                continue
            if not cs:
                ambiguous.discard(i)
                continue
            if len(cs) == 1:
                (key, j), = cs.items()
                push(queue, commit(i, key, j))
                ambiguous.discard(i)
                continue
            sc = sorted(((score(i, k), k, j) for k, j in cs.items()), key=lambda t: -t[0])
            if sc[0][0] > sc[1][0]:
                push(queue, commit(i, sc[0][1], sc[0][2]))
                ambiguous.discard(i)
            else:
                ambiguous.add(i)
        # forced choice among ambiguous devices (symmetric structures)
        best = None
        for i in list(ambiguous):
            if i in done_e:
                ambiguous.discard(i)
                continue
            cs = candidates(i)
            if not cs:
                ambiguous.discard(i)
                continue
            sc = sorted(((score(i, k), k, j) for k, j in cs.items()), key=lambda t: -t[0])
            g, a, b = ext.dev[i]
            nm = sum(x in M and x not in hubs for x in (g, a, b))
            key = (nm, sc[0][0], -len(sc))
            if best is None or key > best[0]:
                best = (key, i, sc[0][1], sc[0][2])
        if best is None:
            # last resort: devices hanging only on hub nodes (e.g. gate unmapped, vss + unmapped)
            for i in range(len(ext.dev)):
                if i in done_e:
                    continue
                cs = candidates(i, allow_hub_only=True)
                if cs and len(cs) == 1:
                    (key, j), = cs.items()
                    push(queue, commit(i, key, j))
            if not queue:
                break
            continue
        forced += 1
        ambiguous.discard(best[1])
        push(queue, commit(best[1], best[2], best[3]))
    return M, {"anchors_used": n_anchor, "wl_seeded_nodes": n_wl, "forced_choices": forced}


def score_mapping(ext: Netlist, gt: Netlist, M: dict):
    gt_count = Counter((G, frozenset((A, B))) for G, A, B in gt.dev)
    ext_count = Counter()
    unmapped = 0
    for g, a, b in ext.dev:
        if g in M and a in M and b in M:
            ext_count[(M[g], frozenset((M[a], M[b])))] += 1
        else:
            unmapped += 1
    matched = sum(min(c, ext_count.get(k, 0)) for k, c in gt_count.items())
    # pullups
    pu_ext_mapped = {M[n] for n in ext.pullups if n in M}
    pu_tp = len(pu_ext_mapped & gt.pullups)
    return {
        "gt_transistors": len(gt.dev), "ext_transistors": len(ext.dev),
        "matched": matched,
        "recall_pct": round(100.0 * matched / max(len(gt.dev), 1), 2),
        "precision_pct": round(100.0 * matched / max(len(ext.dev), 1), 2),
        "ext_devices_with_unmapped_nodes": unmapped,
        "gt_pullups": len(gt.pullups), "ext_pullups": len(ext.pullups), "pullups_matched": pu_tp,
        "nodes_mapped": len(M), "gt_nodes": len(gt.nodes), "ext_nodes": len(ext.nodes),
    }


def unmatched_lists(ext: Netlist, gt: Netlist, M: dict):
    gt_keys = defaultdict(list)
    for j, (G, A, B) in enumerate(gt.dev):
        gt_keys[(G, frozenset((A, B)))].append(j)
    miss_ext = []
    for i, (g, a, b) in enumerate(ext.dev):
        k = (M.get(g), frozenset((M.get(a), M.get(b))))
        if None not in (k[0],) and k in gt_keys and gt_keys[k]:
            gt_keys[k].pop()
        else:
            miss_ext.append(i)
    miss_gt = [j for v in gt_keys.values() for j in v]
    return miss_gt, miss_ext


def compare(ext_path, oracle_path=None, log=print):
    gt, chip = load_gt()
    ext, raw = load_ext(ext_path)
    rep = {"ext": ext_path}
    M, info = match_anchored(ext, gt, log=log)
    rep["anchored"] = {**score_mapping(ext, gt, M), **info}
    if oracle_path and os.path.exists(oracle_path):
        with open(oracle_path) as f:
            O = {int(k): int(v) for k, v in json.load(f).items()}
        rep["oracle"] = score_mapping(ext, gt, O)
        # how often does the anchored matching agree with the oracle on mapped nodes?
        agree = sum(1 for e, g in M.items() if O.get(e) == g)
        rep["anchored"]["node_agreement_with_oracle_pct"] = round(100.0 * agree / max(len(M), 1), 2)
        miss_gt, miss_ext = unmatched_lists(ext, gt, O)
        rep["oracle_missing_gt"] = [chip.trans[j].name for j in miss_gt][:5000]
        rep["oracle_extra_ext"] = [raw["transistors"][i]["name"] for i in miss_ext][:5000]
    return rep, M


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ext", required=True, help="netlist.json from extract.py")
    ap.add_argument("--oracle", help="oracle_map.json (ext node -> GT node) from roundtrip.py")
    ap.add_argument("--report", help="write JSON report here")
    a = ap.parse_args(argv)
    rep, M = compare(a.ext, a.oracle)
    if a.report:
        with open(a.report, "w") as f:
            json.dump(rep, f, indent=1)
    short = {k: v for k, v in rep.items() if not k.startswith("oracle_")}
    print(json.dumps(short, indent=1))


if __name__ == "__main__":
    sys.exit(main())
