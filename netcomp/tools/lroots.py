"""For each dumped lcpu/netsim mismatch event: find 'root' mismatches (no
input gate and no other member of the node's phase component differs) and
check whether they are floating (stored charge) in the reference state."""
import sys, os
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import Netlist
from levelize import Phase
N = Netlist(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../harness/gen/netlist6502.h"))
PH = {0: Phase(N, 0), 1: Phase(N, 1), 2: Phase(N, 1)}
events = open(sys.argv[1]).read().split("end\n")
rootkinds = Counter(); nonfloat_roots = []
for ev in events:
    lines = [l for l in ev.split("\n") if l]
    if not lines: continue
    which = int(lines[0].split()[1]); P = PH[which]
    R, L = {}, {}
    for ln in lines[1:]:
        n, b, r, l, fl = map(int, ln.split()); R[n] = r; L[n] = l
    bad = set(n for n in R if R[n] != L[n])
    def conducting_group(n):
        q = P.cid.get(n)
        if q is None: return [n]
        adj = {}
        for g, a, b, st in P.chan:
            if P.cid[a] == q and (st == 1 or R[g]):
                adj.setdefault(a, []).append(b); adj.setdefault(b, []).append(a)
        seen = {n}; st_ = [n]
        while st_:
            x = st_.pop()
            for y in adj.get(x, []):
                if y not in seen: seen.add(y); st_.append(y)
        return seen
    for n in bad:
        q = P.cid.get(n)
        if q is None:
            nonfloat_roots.append((lines[0], n, "phase-fixed")); continue
        ins = P.inputs.get(q, set())
        if any(g in bad for g in ins): continue           # an input differs: consequence
        grp = conducting_group(n)
        if any(m in bad and m != n for m in grp) and False: pass
        drv = any(R[g] for m in grp for g in P.gnd[m] + P.pwr[m]) or any(P.gnd_const[m] or P.pwr_const[m] for m in grp) \
              or any(N.pullup[m] for m in grp)
        pads = set(N.byname[p] for p in ("rdy","irq","nmi","res","so","clk0")) | set(N.byname["db%d" % i] for i in range(8))
        drv = drv or any(m in pads for m in grp)
        rootkinds["driven" if drv else "floating"] += 1
        if drv: nonfloat_roots.append((lines[0], n, N.name(n)))
print("events", len(events) - 1, "root mismatches:", dict(rootkinds))
for x in nonfloat_roots[:20]: print("  non-floating root:", x)
