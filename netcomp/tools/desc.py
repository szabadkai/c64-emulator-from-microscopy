import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import Netlist
N = Netlist(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../harness/gen/netlist6502.h"))
def nm(n):
    if n == N.vss: return "VSS"
    if n == N.vcc: return "VCC"
    return "%d:%s" % (n, N.name(n))
for a in sys.argv[1:]:
    n = int(a) if a.isdigit() else N.byname[a]
    print("node", nm(n), "pullup" if N.pullup[n] else "no-pullup")
    for i, (g, c1, c2) in enumerate(N.trans):
        if c1 == n or c2 == n:
            o = c2 if c1 == n else c1
            print("   channel t%d gate=%s  other=%s" % (i, nm(g), nm(o)))
    gl = [(i, c1, c2) for i, (g, c1, c2) in enumerate(N.trans) if g == n]
    print("   gates:", ", ".join("t%d(%s-%s)" % (i, nm(c1), nm(c2)) for i, c1, c2 in gl))
