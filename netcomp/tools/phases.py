"""Mechanical clock-phase analysis of the netlist.

Ternary (0/1/X) switch-level propagation with only CLK0 known: every other
pad and every storage node starts as X.  A node whose value comes out 0 or 1
has that value in that phase whatever the rest of the chip holds."""
X = 2


def tor(vals):
    if any(v == 1 for v in vals):
        return 1
    if all(v == 0 for v in vals):
        return 0
    return X


PADS = ["clk0", "rdy", "irq", "nmi", "res", "so"] + ["db%d" % i for i in range(8)]


def phase_constants(N, clk0_level):
    """returns ({node: value}, [nodes in derivation order])"""
    vss, vcc = N.vss, N.vcc
    pads = set(N.byname[p] for p in PADS)
    val = {n: X for n in N.used}
    val[vss] = 0
    val[vcc] = 1
    clk0 = N.byname["clk0"]
    val[clk0] = clk0_level
    chans = {}
    for g, c1, c2 in N.trans:
        if c2 in (vss, vcc):
            continue
        chans.setdefault(c1, []).append(g)
        chans.setdefault(c2, []).append(g)

    def gv(g):
        return 1 if g == vcc else 0 if g == vss else val[g]
    order = [clk0]
    changed = True
    while changed:
        changed = False
        for n in N.used:
            if n in (vss, vcc) or n in pads or val[n] != X:
                continue
            # only nodes that are (now) alone: every channel transistor off
            if any(gv(g) != 0 for g in chans.get(n, [])):
                continue
            gnd = tor([gv(g) for g in N.pulldowns.get(n, [])] + [1] * N.gnd_init[n])
            pwr = tor([gv(g) for g in N.pullups_vcc.get(n, [])] + [1] * N.pwr_init[n])
            if gnd == 1:
                v = 0
            elif gnd == 0 and pwr == 1:
                v = 1
            elif gnd == 0 and N.pullup[n]:
                v = 1
            else:
                v = X
            if v != X:
                val[n] = v
                order.append(n)
                changed = True
    return {n: v for n, v in val.items() if v != X and n not in (vss, vcc)}, order
